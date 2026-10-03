# OP_REGION_MAP Phase 2: run each instruction over many observations

Status: implemented on `feat/region-map`; results in
[cogmod sampling](../performance/2026-10-03-cogmod-sampling.md). Lane
gradients are bitwise equal to the scalar map: each shared-cell addition is
logged per lane and replayed in scalar order after the tile. Revised after
Codex review (2026-10-02). Builds on
[OP_REGION_MAP](2026-10-02-region-map-spec.md).

## Why

Sampling cost per gradient after Phase 1 (`stanli_run`, seed 1, 50+50, one
chain, wall over leapfrogs, same leapfrog counts in every column):

| family | map | old loop | regions |
| --- | ---: | ---: | ---: |
| gamma | 1112 us | 485 us | 852 us |
| weibull | 1078 us | 468 us | 848 us |
| logstudent | 1078 us | 449 us | 812 us |
| exgaussian | 1224 us | 13480 us | 1308 us |
| geg | 2408 us | 25480 us | 2542 us |
| exwald | 4984 us | 44722 us | 5304 us |

Models whose branches flip under sampling are fixed. Models whose branches are
stable lost 2.3x against the old loop. A profile of gamma under sampling:
64% of samples in the register-program interpreter, 19% in the adjoint
sweep, 7% in math functions. Gamma's body is 100 instructions, about 10 of
them arithmetic. The rest is constants, copies, comparison normalization and
jumps, each paying one interpreter dispatch per observation, twice (forward,
then recomputed in the backward).

Two scalar fixes are in progress and carry over unchanged: store each
iteration's registers instead of recomputing them, and run single-write
constants once per evaluation. They cut instructions per observation but not
the dispatch per instruction. This phase removes that: one dispatch per
instruction per group of observations.

## Step A: smaller programs (portable, also helps every region)

Gamma's body after the scalar fixes is 85 instructions, about 10 of them
arithmetic. Two sources of the rest:

- `&&` and `||` are compiled as values: each term emits a comparison, a
  normalization against zero (`NE`), a copy into the result register, and
  jumps; the `if` then tests the result again. Emit conditions as jumps
  instead (a comparison followed by a conditional jump straight to the arm),
  and skip the normalization when the operand is already a comparison or
  logical result. Stan's short-circuit order and domain errors must not
  change.
- `gen_adjoint` writes a path flag at the entry of every block, but only
  blocks with adjoint work get a reverse segment. Write flags only for those
  blocks. Gamma has 20 flags; most guard blocks of the validity chain that
  have no derivative.

## Step B: run each instruction over a tile of lanes

Lanes are iterations. Lanes are processed in tiles of 64. Inside a tile,
per-lane registers are stored register-major (`tile[k * 64 + l]`), so an
instruction's loop over the tile is contiguous and vectorizable. Tiles keep
the working set in cache and keep every lane's forward state for the
backward.

**Register classes**, fixed at lowering:

- Shared: live-in ranges other than the iteration register, and registers
  written only by a hoisted constant. One copy.
- Per lane: everything else, including the iteration register (seeded
  `lo + lane`), checkpoint copies `gen_adjoint` adds, and path flags.
- Adjoint cells are classified by whole `adj_reg` equivalence class: a class
  is shared if any register in it is a shared live-in, so a per-lane copy of a
  live-in still accumulates into the shared cell. Checkpoints and flags have
  no adjoint cell. Check forward and adjoint range homogeneity separately;
  refuse batching if a range mixes classes.
- Refuse batching if any adjoint rule consumes or clears a shared cell (the
  scalar map already refuses this for live-in cells; extend the proof to the
  new classes). `FABS` and `FMIN`/`FMAX` NaN cases assign NaN to an operand
  adjoint; NaN is absorbing, so lane order cannot change the result, but the
  test suite must cover it.

**Forward.** Blocks in program order (acyclic). Each block has a 64-bit lane
mask; the entry block's mask is the tile's active lanes. Each instruction runs
once over its block's mask, dense loop when the mask is full. `JZ` splits the
mask; a block's mask is the OR of what its predecessors sent. Path flags are
ordinary per-lane registers, reset per tile and set on block entry, exactly
as the scalar program does.

- `DYN_INDEX`: per lane, with the scalar path's finite, integral, one-based
  range check including `I.c`.
- Integer ops keep C++ `int` semantics.
- `CALL`, four-argument `DENSITY` and ranged operands: gather the lane's
  operand ranges into a contiguous window, run the scalar code, scatter back.
  Repeated or overlapping CALL operand ranges refuse batching.
- Opcodes not on the list keep the scalar map.

**Target.** Each lane's target register is summed into the output in
ascending lane order, tile by tile, so the forward value is bitwise the
scalar map's.

**Backward.** Tiles in reverse. Within a tile, adjoint segments in their
stored order; a segment's mask is the lanes whose flag is set. Never run a
segment for a lane whose flag is clear, even with a zero seed (`FABS` can
poison). Each adjoint instruction runs over the mask, lanes descending.
Per-lane cells are zeroed per tile; shared cells are seeded once with the
island continuation policy and harvested once, with assignment versus
addition as today.

**Accumulation order.** Per-lane cells and `DYN_INDEX` scatters into distinct
elements are unchanged. A shared scalar that several instructions add to on
every lane is regrouped: instruction-major within a tile instead of
iteration-major. This is within the ULP policy; it can in principle change
overflow (a `+1e308`/`-1e308` pair that cancelled in scalar order). Add
cancellation and overflow cases to the tests.

**Exceptions.** If the batched forward throws, restore inputs and the
prologue, rerun the scalar map forward, and let it throw with the scalar type
and message. If the scalar rerun succeeds, rethrow the original exception.

**Oracle.** The scalar map is the differential oracle: forward values and
per-lane target terms must be bitwise equal; gradients within the ULP policy.

## Expected cost

Gamma, per observation per gradient, from the profile: about 556 ns now. With
the scalar fixes, roughly half the instructions remain and the forward runs
once. Batching removes per-instruction dispatch for the dense blocks; the
remaining per-lane cost is the arithmetic, the math library calls and the
index-list indirection on split blocks. Estimate 150 to 250 ns per
observation, which puts gamma near the old loop. This must be measured, not
assumed: sampling A/B against the scalar map and the old loop, all cogmod
families.

## Native code instead of, or after, Step B

The map gives a native-code backend exactly the shape it wants: one small
acyclic program and its adjoint, run once per observation for every
gradient. The 2026-09-28 probes measured the register VM against the same
programs emitted as C++: 47 vs 21 ns for 16 arithmetic instructions, 35 vs
10 ns for a branch, but 632 vs 554 ns for a 256-instruction dependent chain,
and 256 ms to build with an external compiler. Native code removes dispatch
and keeps scalar semantics bitwise; it needs per-platform code, executable
memory, and does not run in WebAssembly. Step B removes most dispatch
portably and can use SIMD across lanes. A bounded ceiling experiment would
settle the comparison: emit gamma's actual map program and adjoint with the
removed probe emitter (`tools/probe_native_program.cpp`, last at 84945f1b),
build it ahead of time, and measure gamma under sampling.

## Review questions (answered by Codex, folded in above)

1. Is "shared if any register mapped to the cell is shared" the right
   classification for adjoint cells given copy aliasing, and is anything
   written into a shared cell by an adjoint rule other than an accumulation
   (`adj[dst] = 0` clears) that would break when lanes run in sequence?
2. Lane sets at joins: is ascending merge enough, or do adjoint segments ever
   need a lane order other than descending for correctness (not just
   rounding)?
3. Per-lane `CALL`: which CALL kernels keep state across calls (`eval_state`,
   reused call contexts) that per-lane interleaving between forward and
   backward would break?
4. Is there a simpler route that removes the dispatch, for example
   superinstructions or threaded dispatch in the scalar interpreter, that
   should be measured before this?
