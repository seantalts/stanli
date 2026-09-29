# Loop-bearing regions: bounded-memory scans for sequential models

**Date:** 2026-08-29

**Status:** proposed; implementation should land in independently measurable
phases, with the scan opcode and runner proven before lowering selects them

**Problem:** a data-bounded Stan `for` loop is unrolled into the main op graph.
For a sequential recurrence, no vector pass can put it back together: iteration
`t + 1` reads state written by iteration `t`. Heavy matrix-valued bodies then
retain one version of every functional update, plus one necessity island for
every parameter-dependent conditional.

## Decision

Add one graph opcode, `OP_SCAN`, whose payload owns a compiled **one-iteration
step program** and a schedule. The graph sees one op for the complete loop.
The scan kernel owns iteration, reuses the same step register files for every
row, and reverses rows one at a time from compact loop-state checkpoints.

Do **not** encode the complete loop as a back edge in `Program` and do not
replay the complete loop under `stan::math::var`:

- `gen_adjoint` intentionally refuses `JZ`/`JMP`; reversing a flattened jump
  stream loses the structured control needed to select the taken path.
- A whole-loop var replay allocates a tape node per executed body instruction.
  That restores O(iterations x body work) memory at the level below the graph.
- The useful reusable unit is one transition. A C++ scan driver can run that
  transition forward and backward without making the register machine itself
  a general loop runtime.

Runtime conditionals inside one step initially remain the existing
lowering-created `OP_ISLAND` calls. They are therefore differentiated by the
already verified island kernel. A structured branch adjoint can make those
steps cheaper later; it is not a prerequisite for bounding the loop.

### Rejected shortcut: wrap the current `For` in one island

Changing `lower_stmt(For)` to call `lower_runtime_ifelse` around the complete
loop does not implement this design. `ProgramCompiler::stmt(For)` currently
unrolls the loop too, and `stmt(While)` repeatedly emits its body while a
compile-time condition remains true. The allocation merely moves from graph
slots to program instructions/registers. It also meets two existing limits:
the MIR-to-Program front end covers only a fraction of the graph kernel
vocabulary, and any emitted jump makes `gen_adjoint` retain the var replay.

The scan must therefore own the iteration outside `Program`, and its step must
be compiled through the full graph-kernel vocabulary.

## Evidence from issue #248

The attached ctsem data has 4,000 rows, 2,000 subjects, a 1,440-row matrix
setup, 185 parameters, and a 20-dimensional latent population state. The main
loop at source line 577 is a Kalman recurrence; it is neither independent nor
re-rollable.

Scaled data makes the growth visible before the full model exhausts memory:

| data rows | final ops | slot elements | maximum RSS |
| ---: | ---: | ---: | ---: |
| 1 | 12,002 | 18,407,562 | 600 MB |
| 2 | 20,522 | 36,735,423 | 1.03 GB |

At the one-row heap high-water mark, the executor's value, adjoint, and
scratch arenas hold about 369 MB of 437 MB live memory. Final `Op` plus `Slot`
storage is under 2 MB.

The conditional regions explain the scratch side:

| necessity-island property, one-row input | value |
| --- | ---: |
| islands | 3,089 |
| generated-adjoint islands | 10 |
| total forward instructions | 51,005 |
| total registers | 9,414,659 |
| largest register file | 26,186 |
| executor scratch cells attributed to islands | 9,390,110 |

Only a handful of islands contain much code. Most of the memory is repeatedly
snapshotting broad live-in containers around small runtime conditionals.
`STANLI_NO_ISLAND=1` does not help: roughly 3,086 islands are necessities
created by `lower_runtime_ifelse`, not regions selected by the final island
optimization.

This rules out an `Op`/`Slot` byte diet as the ctsem fix. The asymptote has to
change from retaining every unrolled row to retaining one reusable row plan.

## Semantic model

Treat an eligible loop as a state transition:

```text
(carry[t + 1], row_output[t], lp[t]) = step(carry[t], invariant, row_data[t])
```

The scan returns:

- the final values of live-outs read after the loop;
- materialized row outputs for proven disjoint indexed writes; and
- the target contribution accumulated in source iteration order.

Four value classes cross the loop boundary:

1. **Invariant input:** read by the body and never assigned there. Parameters
   and full data arrays normally live here.
2. **Carry:** the value entering iteration `t + 1` depends on iteration `t`.
   State vectors, covariance matrices, `prevrow`, and similar recurrences live
   here.
3. **Disjoint sink:** iteration `t` writes a slice proven not to be read by a
   later iteration and not to overlap another iteration's slice. `llrow[t]`
   is the important example. The final container exists once; it is not part
   of every carry checkpoint.
4. **Reduction:** target increments, and later scalar reductions with the same
   proof, accumulate directly. The first implementation needs only `target`;
   an ordinary scalar assignment can remain carry.

Anything not proven to be one of these classes declines the scan and keeps the
existing lowering. In particular, treating a write-only length-N result as
carry would make checkpoints O(N^2), so uncertain indexed writes must never be
silently classified as carry.

## Artifacts

### `ScanSpec`

Add `runtime/include/stanli/scan.hpp` with an immutable payload owned through
`Graph::udata_pool`:

```cpp
struct ScanSpec {
  struct InputBinding {
    int op_input;
    int input_offset;
    int step_reg;
    int len;
    bool active;
  };
  struct CarryBinding {
    int entry_reg;
    int exit_reg;
    int len;
  };
  struct SinkBinding {
    int step_reg;
    int output_offset;
    int len;
  };
  struct Template {
    IslandProg step;
    std::vector<std::shared_ptr<void>> udata_pool;
    std::vector<InputBinding> inputs;
    std::vector<CarryBinding> carry;
    std::vector<SinkBinding> sinks;
    int target_reg = -1;
  };

  int64_t first;
  int64_t count;
  std::vector<Template> templates;
  std::vector<uint16_t> template_for_iteration;
  int carry_cells;
  int output_cells;
  int checkpoint_block;
};
```

The exact representation may become structure-of-arrays after measurement;
the contracts matter more than this spelling:

- payloads are immutable and shareable across copied executors;
- all mutable evaluation state lives in the owning executor's scan scratch;
- a template has a fixed carry schema even when local shapes require more
  than one body template; and
- schedule entries select templates and data bindings, never source names.

`OP_SCAN` follows the necessity-island packing convention when the logical
inputs exceed `Op::in[6]`: leading inputs are packed with `OP_CONCAT2`, and the
payload retains offsets for individual ranges. This is one bounded graph copy,
not one copy per iteration.

The scan output is one packed slot. Ordinary `OP_INDEX`/`OP_SLICE`
extractions expose final live-outs and sinks. Its last scalar is the complete
loop target contribution and enters `target_terms` once.

### One-step program

The step should reuse graph kernels, not transcribe the remaining 294-op
vocabulary into `mir_prog.hpp`.

Generalize the graph-to-island compiler in `runtime/src/island.cpp` into an
internal `compile_region_program` helper:

- admit vector and matrix outputs;
- emit `Program::CALL` for every pure registered kernel the step uses;
- retain a fragment's `udata_pool`, and add `udata` to `Program::Call` so
  lowering-created `OP_ISLAND` calls keep their payload;
- set `KernelCtx::udata` in both `run_call` and CALL's generated adjoint;
- absorb fragment fills as `CONST`/`CONSTR` pool entries;
- keep the current dead-base aliasing for functional updates; and
- reject `out2`, RNG, print/reject, checks, or another scan initially.

The result is an acyclic `IslandProg` for one iteration. `gen_adjoint` can
differentiate its CALL list because parameter branches remain encapsulated in
their own island calls; no jump appears in the outer step program. Every CALL
backward remains the graph kernel's own backward rule.

This extraction also prevents execution semantics from drifting between a
nested graph runner and `Executor`: the step invokes the same kernel table and
uses the same scratch-size callbacks.

## Lowering and specialization

Selection must happen in `Lowering::lower_stmt`, before the ordinary `For`
case appends any body op to the main graph. Scan construction is transactional:
it builds private fragments and commits one `OP_SCAN` only after every proof
and body compilation succeeds. A refusal discards the private objects and
runs the current unroll unchanged.

### Eligibility, first shipping scope

- data-known finite `for` bounds;
- at least two iterations and a predicted unrolled-cell cost over a small
  threshold;
- log-prob/transformed-parameter path only;
- no RNG, print, reject, validation/check op, external resource, or other
  execute-once effect in the body;
- fixed carry and live-out shapes across iterations;
- no break/continue that depends on a parameter;
- every outer assignment classified as carry, disjoint sink, or target; and
- every indexed sink proven in-bounds, non-overlapping, and unread until after
  the loop.

Nested data loops may remain unrolled inside the one-step fragment. That can
make a step large, but its storage is paid once. Nested sequential loops can
become scans after the outer mechanism is proven.

### Peel exceptional iterations

Always permit a short peeled prefix before the repeated step. This is generic,
not a ctsem special case: initialization-heavy loops commonly give iteration
zero or one a different shape/control signature. The ctsem loop's `rowx == 0`
arm is exactly this case.

Compile and commit a prefix iteration normally, then start the scan at the
first stable carry schema. Cap automatic peeling (initially two iterations);
if no stable schema appears, decline.

### Data schedule and templates

Data are fixed when the model is compiled. Use that fact without baking every
row into graph structure:

1. A data-only prepass evaluates loop-dependent integer state, declaration
   extents, selector lengths, and conditions. This is the same semantic domain
   as `MirInterp`; parameter-dependent expressions remain unknown.
2. The tuple of declaration shapes and statically chosen arms is a
   `SpecializationKey`.
3. Compile one private step fragment per distinct key. Row-varying data values
   and slices are step inputs, not constants in the program.
4. Store only a compact `template_for_iteration` schedule and the row-binding
   descriptors.

The attached ctsem data has one observed `(nobs_y, nbinary_y, ncont_y)` shape:
`(10, 0, 10)` for all 4,000 rows. Apart from the peeled initialization row,
the target should therefore need one fixed-shape template. The mechanism must
still support several templates for other datasets.

The safe bootstrap is incremental fragment lowering plus structural interning:
lower one iteration into a private fragment, normalize relocatable row-data
bindings, and reuse an existing template when graph shape/opcodes match. That
bounds peak memory immediately. The data-only specialization prepass then
removes the remaining O(N x body-compilation) preparation time; the scan is
not considered complete for ctsem until that prepass lands.

## Forward execution

`runtime/kernels/scan.cpp` receives packed invariant inputs and writes the one
packed output:

```text
copy initial carry into current
for t in 0 .. count-1:
    bind template(t) invariant and row-data ranges
    seed the step's carry-entry registers from current
    run_program(step, value_file)
    copy carry-exit registers back to current
    scatter proven row outputs into ctx.out
    add step target to the scan target scalar
    save current at a selected block boundary
write final carry to ctx.out
```

The step value file, its adjoint file, and its kernel scratch ranges are reused
for every row. Row sinks are written directly into their final output range.
No intermediate points into an executor arena, so the payload is safe to share
while each executor owns its scratch.

Forward target additions retain source iteration order. A scan must not use a
parallel or tree reduction: changing that order would introduce a second,
unrelated numerical change.

## Reverse execution and checkpointing

Reverse mode needs the carry entering each row and the row's internal forward
values. Keeping every complete step register file would be O(N x body). Keep
only loop state and recompute the step immediately before differentiating it.

For a block size `B`, scratch contains:

```text
one reusable step value file
one reusable step adjoint file
one reusable step kernel-scratch file
current carry and carry adjoint
ceil(N / B) + 1 persistent boundary carries
B + 1 temporary carries for the block being reversed
```

Backward processes blocks from last to first:

1. Restore the block's saved boundary carry.
2. Replay the block forward, saving each row boundary into the temporary carry
   buffer.
3. For each row in reverse order, rerun that one step to rebuild its value and
   kernel-scratch files, seed final-carry/sink/target adjoints, run the step's
   generated adjoint, and harvest the carry-entry and invariant-input
   adjoints.
4. Carry the harvested state adjoint into the preceding row. Accumulate
   parameter adjoints directly in reverse row order.

Recomputation is legal only for effect-free deterministic steps, which is why
the first eligibility gate excludes messages, RNG, and checks. Given identical
inputs, the existing double kernels reproduce the same bits; branch islands
therefore take the same arm during recomputation.

Use two checkpoint modes:

- If all `(N + 1) x carry_cells` boundary values fit under a configurable
  budget, retain them. Backward then needs one extra row forward per row.
- Otherwise choose `B` near `sqrt(N)`, minimizing
  `(ceil(N / B) + B) x carry_cells`. This adds one block replay, keeping memory
  O(sqrt(N) x carry) and work near three forwards plus one backward.

Expose `STANLI_SCAN_CHECKPOINT_MB` for measurement and constrained hosts, and
`STANLI_NO_SCAN=1` as the semantic/performance oracle. A Revolve schedule is a
possible later refinement; the square-root schedule is simple enough to audit
and already changes the asymptote.

## Correctness arguments

### Values

The step fragment is ordinary lowering over one iteration, followed by the
same graph passes and the same kernels. Carry writes may reuse storage only
where the loop liveness proof says the prior version cannot be observed.
Disjoint sinks write the final buffer position the unrolled functional update
would leave behind. Target terms are added in the same iteration order.

### Gradients

The unrolled graph's backward visits rows in descending order. The scan does
the same. Within a row, `run_adjoint` visits step instructions in the same
reverse order, and CALL delegates to the same kernel backward. Carry adjoints
connect row `t + 1` to row `t`; invariant parameter contributions accumulate
as each row unwinds. A sink's final output adjoint seeds only the row that
wrote its proven-disjoint slice.

This ordering is sufficient for bitwise agreement except where an already
documented kernel path has its own tolerance. The scan itself introduces no
new reassociation.

### Repeated evaluation

All scratch, boundary checkpoints, current adjoints, and step adjoints are
cleared or overwritten on every evaluation. Tests must run two gradients on
the same executor and on two copied executors; stale carry and double-counted
adjoints are the likely failure modes.

## Memory model

Let:

- `G` be the fixed main graph outside the loop;
- `V`, `A`, and `S` be one step template's value, adjoint, and scratch cells;
- `C` be loop-carried cells;
- `O` be semantically materialized loop outputs; and
- `N` be iterations.

Current lowering is approximately:

```text
O(G + N x (V + A + S))
```

The scan with square-root checkpoints is:

```text
O(G + V + A + S + O + sqrt(N) x C)
```

The ctsem one-row profile says `V + A + S`, not graph structs, is the large
constant. The scan does not make that row cheap in its first phase; it makes
the row storage reusable so 4,000 rows do not multiply it.

## Implementation phases

### Phase 0: diagnostics and refusal accounting

- Add preparation counters for candidate loops, refusal reasons, estimated
  unrolled cells, templates, carry/sink/output cells, and checkpoint cells.
- Add a graph-cell high-water diagnostic before an allocation request, so a
  refused scan explains the remaining risk rather than looking hung.
- No behavior change.

### Phase 1: scan kernel and manually built plans

- Add `ScanSpec`, `OP_SCAN`, kernel registration, scratch sizing, forward, and
  backward.
- Generalize graph-to-Program CALL compilation, including `udata` propagation.
- Construct plans directly in `tests/test_scan.cpp`; lowering does not select
  them yet.
- Prove scalar and matrix recurrence, sink routing, target accumulation,
  checkpoint modes, parameter branches, and repeated gradients.

### Phase 2: fixed-shape early lowering

- Add transactional loop analysis and private one-iteration fragment lowering.
- Support invariants, carry, target, and affine disjoint scalar/slice sinks.
- Peel a short prefix and retain current unroll as the fallback.
- Activate behind the normal `STANLI_NO_SCAN` opt-out.

### Phase 3: data specialization and ctsem

- Add data-only schedule extraction and template interning.
- Lift row-varying data values/slices into step inputs instead of constants.
- Support data-only integer carry and stable multi-template schedules.
- Run the complete attached ctsem model; this phase is not done at N=1 or N=2.

### Phase 4: reduce the one-row constant

- Coalesce adjacent necessity regions inside a step so broad live-ins are
  snapshotted once per lexical region, not once per small conditional.
- Generate structured branch adjoints from MIR `if` form, or record/reverse a
  compact taken-path trace. Preserve the existing island replay as the oracle.
- Add a step-local register allocator only after profiles show its payoff.

### Phase 5: wider loop coverage

- Nested scans and data-dependent `while` where a finite runtime guard exists.
- More sink forms and proven reductions.
- Checkpoint scheduling beyond square-root blocks if recomputation dominates.

Each phase is separately shippable except Phase 3's ctsem acceptance target;
do not hide the full-model requirement behind scaled-data success.

## Verification gates

### Focused tests

- `N = 0`, `1`, `2`, and a non-block-aligned large count;
- scalar recurrence with a parameter used every row;
- vector/matrix carry updated by functional assignments;
- target term plus final carry output;
- disjoint scalar, contiguous-slice, and strided-slice sinks;
- refusal for overlapping or later-read sink writes;
- a parameter-dependent branch taking both arms;
- a peeled first iteration followed by a stable template;
- two fixed-shape templates selected by row data;
- full-boundary, `B = 1`, and square-root checkpoint schedules;
- repeated gradients, copied executors, and multiple scan ops in one graph;
- every execute-once effect refused before the main graph mutates.

Every focused semantic test compares feature-on with `STANLI_NO_SCAN=1` at
the same point. Prefer bitwise comparison; explain and ledger any exception.

### Repository gates

- the full native test suite and sanitizer targets touching lowering,
  executor, islands, adjoints, write-array, and cross-path behavior;
- `tools/verify_refs.py` over the PosteriorDB corpus;
- feature-on/off graph and gradient A/B for every model where a scan fires;
- preparation and gradient timing in alternating order; and
- a scan activation audit showing exactly which loops changed and why every
  declined loop declined.

### Issue #248 acceptance

1. The constant-target reproducers remain two-op graphs.
2. On scaled ctsem data, final main-graph ops and executor arena cells stop
   growing linearly with row count.
3. The complete 4,000-row attachment compiles and evaluates on a machine that
   previously killed it near 35 GB.
4. Report preparation time, first-gradient time, steady-state gradient time,
   maximum RSS, step-template count, carry cells, output cells, and checkpoint
   cells.
5. Compare lp and all 580 gradients with the existing unrolled path at the
   largest row count that path can safely run, then with CmdStan on the full
   data.

## Expected files

- `runtime/include/stanli/scan.hpp`: immutable scan and template contracts.
- `runtime/src/scan.cpp`: loop analysis, fragment normalization/interning, and
  preparation statistics.
- `runtime/kernels/scan.cpp`: scratch sizing, forward, blocked recomputation,
  and backward.
- `runtime/include/stanli/program.hpp`, `runtime/src/adjoint.cpp`, and
  `runtime/src/executor.cpp`: CALL `udata` and shared region-program support.
- `runtime/src/island.cpp`: extract the reusable graph-to-Program compiler.
- `runtime/src/lower.cpp`: transactional early `For` selection, peeling,
  scope commit, and target/live-out extraction.
- `runtime/include/stanli/optable.hpp`: `OP_SCAN` registration.
- `tests/test_scan.cpp` plus a small MIR fixture: focused semantics and memory
  scaling.
- `runtime/src/OPTIMIZATIONS.md`, `docs/hacking.md`, and `CHANGELOG.md`: the
  final proven behavior and measurements, written only when a phase ships.

## Non-goals

- A general JIT or a second math implementation.
- Parallelizing a sequential recurrence.
- Making all dynamic Stan loops scan-eligible in the first release.
- Replacing the graph executor, MIR interpreter, or existing necessity-island
  fallback.
- Shrinking `Op`/`Slot` as a substitute for changing the loop asymptote.

The design succeeds if the loop body remains semantically ordinary stanli
code, but its storage lifetime becomes one reusable transition instead of the
entire unrolled history.
