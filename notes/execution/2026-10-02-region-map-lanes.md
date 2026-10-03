# OP_REGION_MAP Phase 2: run each instruction over many observations

Status: design for review. Builds on [OP_REGION_MAP](2026-10-02-region-map-spec.md).

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

## Shape

Lanes are iterations. One evaluation runs the program once over all lanes.

**Register classes**, fixed at lowering:

- Shared: live-in ranges (immutable, already checked) and registers written
  only by a hoisted constant. One copy.
- Per lane: every other register, including the iteration register and the
  adjoint path flags. Stored lane-major per register: `lane_reg[k * L + l]`
  for per-lane register index `k` and lane `l`. Per-lane indices are assigned
  so that every range an instruction reads or writes stays contiguous.

Refuse batching (keep the scalar map) if any instruction's range mixes the two
classes.

**Forward.** The program is acyclic with forward jumps only. Walk blocks in
program order. Each block has a lane set; the entry block has all lanes.

- Each instruction runs once over its block's lane set. Operand addressing is
  resolved at lowering: a shared operand is one value, a per-lane operand is a
  base plus lane. A full lane set uses a dense loop with no index list.
- `JZ` splits its block's lane set by the condition into the fall-through and
  the target. A block's lane set is the ascending merge of what its
  predecessors sent it. `JMP` sends the whole set.
- `DYN_INDEX` gathers per lane: the index register is per lane (usually the
  iteration register or derived from it), the indexed run is shared (a live-in
  vector) or per lane (a body-local vector).
- `CALL` runs per lane: gather the lane's input ranges into a contiguous
  window, call the kernel's scalar forward, scatter the output and the
  kernel's scratch back to per-lane storage.
- Opcodes are on an explicit list (arithmetic, comparisons, `JZ`/`JMP`,
  constants, `MOV`/`MOVR`, `DYN_INDEX`, the scalar unary math ops, `LOG_MIX`,
  `LSE2`, `LOG_DIFF_EXP`, `FMA`, `POW`, `FMIN`/`FMAX`, `IMOD`/`IDIV` and the
  integer ops, `DENSITY`, `CALL`). Any other opcode keeps the scalar map.

The per-lane register file holds every lane's forward state when the forward
ends, so the backward recomputes nothing.

**Backward.** Adjoint cells split the same way. A cell is shared when any
forward register mapped to it (`adj_reg`) is shared; that includes a copy that
shares a live-in's cell. Otherwise per lane.

- Zero the per-lane adjoint storage once per backward. Seed shared live-in
  cells with the island continuation policy, as the scalar map does. Seed the
  target cell of every lane with the output adjoint.
- Walk the adjoint segments in their stored (reverse) order. A segment's lane
  set is the lanes whose guard register is nonzero. Without segments, all
  lanes.
- Each adjoint instruction runs once over the segment's lanes, lanes in
  descending order. Per-lane cells are independent. Shared cells receive one
  addition per lane in that order.
- `CALL` backward per lane with gathered value and adjoint windows.
- Harvest shared live-in cells as today.

## Accumulation order

The scalar map adds into a shared cell iteration by iteration in reverse, and
within an iteration in reverse instruction order. The batched backward adds
instruction by instruction, and within an instruction lane by lane in reverse.
For a cell that one instruction writes once per lane (the usual case: `mu[n]`
through `DYN_INDEX`, where each lane hits its own element) nothing changes.
For a shared scalar that several instructions add into on every lane (a
broadcast parameter), the sum is regrouped. That is within the ULP policy
(bitwise is not a gate), and the corpus replay bounds it.

## Exceptions

A domain error at lane `i` in instruction `j` would surface from the batched
forward at the first instruction where any lane fails, which can be a
different lane and message than the scalar order. On any exception from the
batched forward, rerun the scalar map forward from the start and let it throw.
The cost only applies to rejected evaluations.

## Expected cost

Gamma, per observation per gradient, from the profile: about 556 ns now. With
the scalar fixes, roughly half the instructions remain and the forward runs
once. Batching removes per-instruction dispatch for the dense blocks; the
remaining per-lane cost is the arithmetic, the math library calls and the
index-list indirection on split blocks. Estimate 150 to 250 ns per
observation, which puts gamma near the old loop. This must be measured, not
assumed: sampling A/B against the scalar map and the old loop, all cogmod
families.

## Open questions for review

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
