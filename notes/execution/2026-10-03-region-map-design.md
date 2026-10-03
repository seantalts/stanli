# OP_REGION_MAP

One graph op that runs a likelihood loop's body over every observation. The
body is compiled once into a register program; the op runs that program for
each value of the loop variable and adds up the target terms. "Map" in the
functional sense: the same function applied to every observation.

```stan
for (n in 1:N)
  target += cogmod_exgaussian_lpdf(Y[n] | mu[n], sigma[n], tau[n]);
```

becomes one `OP_REGION_MAP` whose inputs are `Y`, `mu`, `sigma`, `tau`, and
whose output is the sum of the N terms.

## Why it exists

Before it, a loop whose body branches on parameters had two lowerings.

- Per-observation regions: the graph is unrolled, and every observation's
  branch becomes its own small compiled program (`OP_ISLAND`). Correct under
  any branch pattern, but compiling N programs is slow (invgaussian spent
  115 s preparing) and each island is a separate graph op.
- The structured loop (`OP_LOOP`): records one pass through the loop and
  replays it. Fast while every observation keeps the branch it took during
  recording. When any observation's branch changes, the whole loop is
  re-recorded at about 30x the cost of a replay. Under NUTS the parameters
  move every step, observations near a threshold flip, and families like
  exgaussian re-recorded on nearly every gradient. #423 routed more loops to
  this engine, which made them about 10x slower under sampling.

The map compiles once, never records, and does not care which branch each
observation takes.

## When it is chosen

In `Lowering::lower_stmt_impl`'s `for` case, before the structured loop, a
loop maps when all of these hold (`runtime/src/lower_region_map.cpp`):

- Fixed bounds, at least 32 iterations, log density only (not generated
  quantities).
- The body branches on a parameter: an `if`, ternary, `&&`/`||` or `while`
  condition that depends on a parameter, followed into user functions with
  each argument's dependence taken from the call site. A UDF that branches on
  `y == 0` where `y` is data does not count.
- Iterations are independent: everything the body assigns is declared inside
  it, nothing escapes with `break`/`return`, and the body does not read
  `target()`. A recurrence (`state = a * state + c`, the shape of ctsem's
  filter) is refused and stays on the structured loop.
- No effects (print, reject, RNG).
- The region compiler accepts the body and a native adjoint can be generated.

Anything refused lowers exactly as before. `STANLI_REGION_MAP=0` turns it off.

## Compilation

The body goes through the same `ProgramCompiler` as runtime-control regions,
with three differences:

- The loop variable is a register, not a compile-time integer, so `Y[n]`
  compiles to `DYN_INDEX` over the whole `Y` vector. Whole vectors are
  live-ins, copied into the register file once per evaluation.
- `target +=` accumulates into a register that starts at zero, so one run of
  the program produces one observation's term.
- Constants written exactly once move to a prologue that runs once per
  evaluation.

`gen_adjoint` then generates the reverse program. Supporting the map needed
native adjoints for `DYN_INDEX`, `IMOD` and `IDIV`, a definite-initialization
analysis that is linear in program size, and compiling `&&`/`||` conditions as
jumps (gamma's body went from 85 to 56 instructions).

## Execution

Three forms, chosen at lowering:

1. Lanes (20 of the 21 mapped cogmod families). Observations are processed in
   tiles of 64. Inside a tile each per-observation register is a row of 64
   values, and each instruction runs once over the tile instead of once per
   observation. Branches split a 64-bit lane mask; a block runs for the lanes
   that reach it and is skipped when none do. Live-ins and hoisted constants
   are shared, not copied per lane. The forward keeps every tile's registers
   for the backward, which walks tiles in reverse and runs the adjoint the
   same way.
2. Scalar save: one observation at a time; the forward saves each
   observation's registers, the backward restores them and runs the adjoint.
   Used when the lane plan refuses (an opcode it does not batch).
3. Scalar recompute: the backward reruns each observation's forward instead
   of saving it. Used when the saved state would be too large (invgaussian's
   body unrolls a 64-point quadrature to 57k instructions).

`STANLI_REGION_MAP_DIAGNOSTICS=1` prints which form each loop got and why.

## Exactness

Each observation computes the same scalar operations as before, so values
are unchanged. Two sums could have been regrouped:

- The output adds the observation terms in order, then adds that total to the
  rest of the target. That reassociates against summing every term into the
  running target, within the ULP policy.
- In lanes, several observations add into one shared adjoint cell (a scalar
  parameter every observation reads). Each lane's additions are logged and
  replayed after the tile in the scalar order, so lane gradients are bitwise
  equal to the scalar map's.

Every rule in the generated adjoint clears the cell it consumes, so after one
observation's (or one tile's) backward every cell is zero again apart from
live-in accumulators and cells nothing reads. `STANLI_REGION_MAP_CHECK_CLEAN=1`
checks that after each observation or tile; the cogmod ctest entries run with
it.

## Code

- `runtime/src/lower_region_map.cpp`: selection, compilation, form choice.
- `runtime/kernels/region_map.cpp`: scalar forward and backward.
- `runtime/kernels/region_map_lanes.cpp`: lane plan, tile forward and
  backward.
- `runtime/include/stanli/region_map.hpp`: the payload.
- `runtime/include/stanli/adjoint_rules.hpp`: adjoint rules shared by the
  scalar sweep and the lanes.

## Limits

- Memory: lanes keep every tile. lnr_bench peaks at 138 MB resident against
  84 MB without the map.
- LNR is 1.5x behind CmdStan: its body is 3227 instructions, mostly branches
  that split the lane mask.
- `LSE_RANGE` and other ranged ops are not batched, so invgaussian runs the
  scalar form.
- Loops that carry state between iterations stay on the structured loop.

Earlier documents: [spec](2026-10-02-region-map-spec.md),
[lanes](2026-10-02-region-map-lanes.md),
[results](../performance/2026-10-03-cogmod-sampling.md).
