# Native within-chain reduce_sum parallelism

Status: design revised after [Fable review](../plans/2026-09-17-native-reduce-sum-fable-review.md).
The review supports a bounded prototype, not production approval. The
[first feasibility experiment](../plans/2026-09-18-native-reduce-sum-prototype.md)
is complete with numerical checks and measured speedups. Production runtime
behavior remains unchanged. Inspected baseline:
`be0a0c8d4882afca3e1d1de0083267df10f8fb3d`, clean
detached worktree before this document. This checkout has neither fetched native
dependencies nor a build directory at design time. The subsequent prototype uses
a clean Release build; its report records dependency provenance.
Updated 2026-09-18 after user review: make up-front allocation and direct writes
into planned storage explicit. The user's clarified priority is good performance
with practical peak memory and rare OOMs, not a zero-allocation guarantee.
These memory-layout revisions have not been reviewed by Fable.

## Recommendation

Add an opt-in native retained reduction operation. Compile each partial-sum
callback into an independent child graph, execute chunks with a bounded persistent
worker team, then combine values and input derivatives in a fixed order. Preserve
the current whole-slice lowering when the feature is disabled or a call is
ineligible. Start with statically known slice geometry and grainsize, and keep
reductions inside runtime regions and interpreters serial in the first version.

Reuse the retained-loop machinery's transactional child lowering, import
coalescing, packing, and per-executor state ownership. These are existing
building blocks. The missing pieces are the retained reduction boundary,
scheduling, deterministic merge, and an explicit callable reverse interface.

The first experiment should establish whether native child graphs have enough
headroom after dispatch, packing, retention, and gradient merging. Do not begin
with a general parallel graph scheduler or assume that linking TBB enables this.

## Existing seams and constraints

- `runtime/src/lower_higher_order.cpp:576`: `lower_reduce_sum` evaluates the
  arguments, validates grainsize, and rewrites to `f(slice, 1, N, shared...)`.
  The parallel boundary disappears during lowering.
- `runtime/include/stanli/mir_prog.hpp:1804` and
  `runtime/include/stanli/mir_interp.hpp:1224` independently implement serial
  reductions in runtime programs and interpreted phases.
- `runtime/include/stanli/graph.hpp:146`: an Executor owns mutable values,
  adjoints, scratch, and kernel state. Copies rebind independent contexts.
- `runtime/include/stanli/optable.hpp:881`: kernels can have immutable graph
  payloads and separate `KernelState` instances per bound operation.
- `runtime/include/stanli/structured_loop.hpp:89-99` and
  `runtime/src/lower_structured_loop.inc:1995-2184`: retained loops already have
  child graphs/fills, activity-aware imports, same-slot alias coalescing,
  six-input packing, and isolated trial lowering. Factor/reuse those contracts.
  `runtime/src/structured_loop.cpp:2140-2164` supplies an existing seeded reverse
  and saved-state lifetime model, although ordinary Executor lacks that API.
- `runtime/src/executor_pool.cpp:7` and `runtime/src/nuts.cpp:299`: native worker
  threads explicitly construct `stan::math::ChainableStack`. The existing
  ExecutorPool leases whole-model executors; it is not a task scheduler.
- `CMakeLists.txt:141`: native builds normally define `STAN_THREADS`; wasm does
  not. TBB symbols are stubbed. `STAN_THREADS` supplies TLS safety, not reduction
  scheduling.
- `runtime/include/stanli/kernel_types.hpp`: `Op` and `KernelCtx` have six input
  slots. Arbitrary shared callback arguments need packing or a separate binding
  descriptor, not silent truncation or widening every hot operation.
- `runtime/src/executor.cpp:43`: values-only evaluation is thread-local state;
  dispatch to another thread must explicitly propagate evaluation mode.
- `tests/test_reduce_sum.cpp`: current serial parity is exact, including
  argument evaluation, empty slices, shapes, overloads, and normalization.

Stan permits regrouping the sum. Static reductions require a reproducible
partition with grainsize as the maximum chunk size; ordinary reductions treat
grainsize as a scheduling hint. Absolute bounds are one-based and inclusive,
while the sliced argument starts locally at index one. See the
[Stan parallelization contract](https://mc-stan.org/docs/2_38/stan-users-guide/parallelization.html).
Neither partition nor last-bit equality with CmdStan's scheduler is promised.

## Lowering and eligibility

Introduce a proposed `ReduceSumSpec` and `OP_REDUCE_SUM`, with existing lowering
as the disabled/default path. New identifiers in this document are proposals.
Spec construction runs transactionally in a separate child lowering context.
Commit parent slots, bindings, and payloads only after every chunk succeeds.
On refusal, the parent graph and all compiler state must equal disabled lowering.

Initial eligibility requires known outer length, element shape, and a safely
foldable positive data grainsize; resolved callback and full normalization/activity
metadata; no transitive print, explicit reject, RNG, target mutation/read, or
other ambient-state effects in the callback; and child graph lowering that can
prove independent state ownership. Parameter-dependent branches are allowed only
where existing child lowering supports them correctly. Otherwise retain serial
behavior. This is a structural rule, with no model-name or source-text matching.

Check `region_current` before attempting or emitting a retained reduction.
When non-null, immediately use the existing serial rewrite. Otherwise a new
stateful child kernel would make retained-loop preparation refuse the enclosing
region (`structured_loop.cpp:133-148`), potentially breaking an existing model.
Reduction children use ordinary graph execution semantics: the prototype uses
owning Executors, and the production layout binds contexts to reserved arena
ranges as described below. Children may contain supported islands and retained
loops with their own private states. Reusing import/trial
construction does not mean passing arbitrary child graphs through
`StructuredLoop::prepare`, which rejects islands, nested OP_LOOP, and stateful
body kernels. Parallel reductions are disabled recursively during child lowering.

The existing effect classifier is a starting point, not a proof of thread safety:
audit callbacks and kernel payloads for mutable shared caches and thread-local
scratch reentrancy. Domain errors in otherwise pure arithmetic remain allowed;
the failure protocol below must handle them. Nested reductions execute their
existing whole-slice path inside a child; they never submit nested tasks.

All actual arguments are evaluated exactly once on the owner before dispatch,
including unused shared arguments and empty slices. Preserve the established
argument/validation phase: do not prevalidate before shared argument effects.
Zero-length slices validate and return zero without compiling/executing the body.
Effects in argument expressions need not disqualify a pure callback if their
results are safely materialized once. Non-foldable grainsize stays serial in v1.

Child compilation binds the local slice shape, absolute start/end, shared
argument shapes, per-formal data-only rules, and `_lpdf` versus `_lupdf`/`_lpmf`
versus `_lupmf` normalization exactly as the existing UDF path. Integer and
inactive real formals must not accidentally become autodiff values when packed.
Bounds slice the outer array dimension; inner matrices retain their storage order.

For v1, specialize child graphs per chunk, including absolute bounds. This avoids
pretending the current fixed-shape graph supports arbitrary runtime subviews.
Code duplication and shared-data replication are explicit costs to measure, not
assumed free. No compilation occurs per gradient. A later reusable callback
template is a separate optimization needing runtime bounds/shape support.

Use the retained-loop import rule: coalesce identical parent slots into one
import, OR their activity, AND their data-only classification, and let formal
bindings reference that import. Carry per-formal normalization/type metadata
independently. Pack overflow inputs through the existing concatenation scheme
and keep explicit import offsets/activity, including inactive/integer ranges
inside a packed operand. Distinct graph slots that came from overlapping slices
remain distinct imports; their existing parent gather/scatter operations combine
the derivatives. Never coalesce based on coincident numeric values. No parent
arena pointers live in immutable payloads. Reuse these contracts rather than
inventing an independent per-formal merge order; the final owner merge iterates
chunks then canonical imports. Measure large shared-input copies explicitly.

Keep the new operation opaque to parent graph rewrites in v1. Add it to the
no-fold/no-reordering boundaries in `is_effectful_op`; this is an execution-phase
constraint even though eligible callbacks have no explicit user effects. Reject
conversion into ordinary `Program::CALL`, whose current binding does not create
`KernelState`. Audit in-place forwarding, slot remapping, rerolling, island
carving, and liveness. Prefer payload mappings expressed as operand index plus
offset, not outer slot IDs; otherwise every slot-renaming pass needs a remapper
or explicit refusal. Child graphs still undergo their ordinary supported passes.
Also preserve structural serial dispatch in `higher_order_eval.cpp` and the MIR
interpreter: its generic kernel helper can instantiate stateful kernels, so
absence of state there is not an adequate accidental safeguard.

## Partition and execution policy

Keep `threads_per_chain=1` as the public default. Explicit preparation options
enable retained reductions; runtime options select the number of workers for that
prepared plan. Preparation is necessary because today's serial lowering erases
the boundary. Existing creation APIs keep their behavior; add a versioned options
entry point rather than extending a stable C struct without an ABI change.
Existing `num_threads` remains the parallel-chain setting. Python/R can present a
single `threads_per_chain` option while arranging preparation internally, and
must explain when a new prepared model is needed.

For `reduce_sum_static`, partition into fixed contiguous chunks of at most
grainsize elements, independent of available workers. For ordinary `reduce_sum`,
start with a deterministic partition selected at preparation using grainsize and
a declared target concurrency; dynamically assign these fixed tasks to workers.
For grainsize one, coarsen to a small multiple of target concurrency rather than
creating one graph/task per observation. Record the effective partition in
diagnostics. Adaptive partition changes are deferred.

An already-prepared retained plan with one execution worker still executes its
fixed partition. It is the scheduling oracle, distinct from a default serial
model, which retains whole-slice evaluation. Do not switch partition merely
because workers are temporarily unavailable. Static reproducibility applies
within the same prepared plan/build, not between disabled and enabled modes.

Use a persistent team owned by the sampling/evaluation context, with RAII
lifetime. The owner participates, so P means at most P executing threads for a
chain. A first native sampler integration can use one team per active chain,
with total concurrency explicitly bounded by active chains times
threads_per_chain; expose the effective total and do not silently multiply the
existing parallel-chain count. External concurrent callers supply/share an
explicit execution context or accept their documented separate resource budgets.
Do not use an unbounded process-global pool or spawn threads per gradient.

Concrete plumbing: add a non-owning `ReduceExecutionContext*` to EvalState,
carrying the caller-owned team, cancellation token, and dispatch-depth policy.
Add internal Executor entry overloads for gradient and forward that accept this
state; old overloads use serial defaults. Keep the state installed through BOTH
forward and reverse, and ensure nested forward helpers do not reset it. Samplers
create one context/team per active chain; external callers supply a context per
evaluation lease. Neither KernelState nor ExecutorPool creates worker teams.
Child executions carry a serial/depth-marked context and cannot submit again.
The team must outlive the complete evaluation and every task it submitted.

Without TLS-safe native support, use the serial implementation and report the
effective capability. Wasm remains serial within each browser worker.

## Storage layout and allocation lifetime

Prefer allocating predictable execution storage when binding a chain's executor
and execution context, then reusing it across gradients. Avoid repeatedly
constructing/cloning child executors or churning task and numeric buffers.
Preassemble contexts, offsets, and synchronization resources where useful.
Allow lazy setup and capacity growth when their measured cost is small or they
avoid excessive up-front reservation. Reset contents/readiness between
evaluations; resetting an arena cursor or clearing adjoints is not allocating a
replacement arena. These are performance choices, not eligibility requirements
or a promise that every gradient performs zero allocations.

The preferred production layout is a planned set of chain-owned typed arenas
with non-overlapping writable ranges for chunks. Child execution contexts are
views into those ranges; workers write directly to their assigned addresses.
Logical independence does not require a separate heap allocation per worker or
per chunk, and no whole-arena merge occurs after execution. The existing Executor
owns its vectors, so implementing this layout needs an internal storage-view
binding interface (or an equivalent subgraph executor); it is not supported by
the current constructor. The feasibility harness may use ordinary child
Executors constructed once before evaluation, but must distinguish that layout
from the proposed production layout and count their storage accurately.

Share immutable data and read-only parent inputs wherever their lifetime and
kernel access contracts allow. Snapshot only inputs whose values cannot remain
valid until their last child read, such as values subject to parent in-place
overwrites before reverse. Take any required snapshot into reserved storage.
Do not replicate the whole parent model or every shared data array per thread.

Plan storage by live ranges, not merely by worker count. Forward values needed
for reverse belong to the chunk and survive even if that worker executes another
chunk. Temporary workspace can be reused after its last consumer. Separate
reduce_sum calls may reuse ranges only when forward/reverse lifetimes do not
overlap; execution order alone is insufficient. Scheduling cannot change the
ownership or numerical grouping of retained results.

This uses the same up-front arena-planning principle as serial execution, but
does not guarantee the same byte count or unchanged serial offsets. Temporaries
reused by sequential work can be simultaneously live in parallel work. Shared
gradient contributions also need separate storage until reduction. Report that
incremental memory rather than multiplying the entire serial arena by P.

Judge storage choices by end-to-end gradient performance, preparation cost, and
peak memory across concurrent chains. Allocation counts are diagnostic, not a
pass/fail gate. Existing nested AD, solver, and dynamic retained-loop paths can
allocate or grow storage; this alone is no reason to refuse parallel execution.
Do not reserve a speculative worst-case tape merely to remove rare growth.
Retain useful capacity within a practical memory budget, and allow releasing
exceptional high-water allocations between evaluations. Growth must preserve
bound-pointer validity: use stable blocks or rebind safely before dispatch,
never resize storage that an active task is using.

Estimate aggregate memory across chunks and active chains before choosing a
plan. If it is excessive, consider fewer simultaneously live chunks, bounded
batches, or a smaller preparation-time partition where the reduction contract
allows it; preserve static grainsize and deterministic grouping. An explicit
serial fallback is preferable to predictably excessive reservation. No policy
can guarantee against OOM under arbitrary process load, but memory growth must
be measured and failures must unwind cleanly rather than leave corrupt state.

## Forward, reverse, and ownership

The reduction's `KernelState` owns or references the prebound chunk contexts,
saved forward ranges, result/error records, and mapping buffers described above.
Immutable callback graphs may be shared. Concurrent executions have disjoint
writable storage. Parent executor copies bind fresh mutable reduction storage
before evaluation; they do not clone it on each call.

Forward binds shared read-only inputs and any required preallocated snapshots,
then runs child forwards in parallel. Preserve enough values and kernel scratch
for later reverse despite parent in-place overwrites. After all jobs finish
successfully, the owner combines chunk scalar results in ascending chunk order
and publishes one scalar. Workers never concurrently add to the parent target.

Reverse runs independent child reverse sweeps with the actual incoming scalar
seed, then the owner accumulates child input derivatives in ascending chunk and
canonical import order. Adapt the retained-loop seeded-adjoint contract into an
internal seeded reverse-after-forward interface for ordinary child Executors; the
existing `gradient()` recomputes forward and only exposes derivatives of model
parameters, so it is not the required callback API. Bind arbitrary active
callback live-ins explicitly and seed the child's scalar return. Avoid computing
unit gradients eagerly in forward and later scaling for arbitrary uses: that
changes rounding and can mishandle zero/nonfinite seeds. Cover inactive and
zero-width inputs. Fable proposed a useful special case: when graph-use analysis
proves the output's reverse seed is exactly +1 (for example an unscaled direct
target contribution), fuse child forward+reverse in one dispatch using existing
gradient calls. Prototype that case first. Integration also needs proof that
moving derivative evaluation preserves failure/effect timing; a syntactic
`target +=` check alone does not prove the complete contract. General uses retain
the seeded two-phase path.

For input adjoints with proved disjoint destination ranges and no conflicting
aliases, workers may write directly into those reserved destinations. Shared
parameters are different: if every chunk uses beta, every chunk contributes to
the same beta adjoint. Store those contributions in preallocated chunk ranges
and add them to the parent in deterministic order. This is a reduction of shared
derivatives and scalar partial sums, not a merge or copy of all worker arenas.
Direct writes also need an error-publication contract: the whole evaluation is
invalid on failure, no gradient is returned, and the next evaluation resets any
partially written adjoints. Until these ownership/alias/lifetime facts are
proved, use private preallocated contributions and the owner merge.

No `stan::math::var`, `vari*`, nested AD arena, or thread-owned scratch reference
crosses a task boundary or survives for use on another worker. Existing legacy
kernel and island replay tapes must be opened, used, and recovered wholly on the
executing worker. Persist only executor-owned numeric state. Install worker TLS
AD stacks at worker entry. Before submission the owner captures evaluation mode;
each worker explicitly selects child `forward_value_only()` or ordinary forward
from that captured mode, never from the worker's default TLS flag. Data/value
only runs must not allocate derivative state unnecessarily or change propto
behavior. Audit nested-call TLS scratch reuse, not just cross-thread races.

The values-only guard must save and restore its previous value. Today's guard
unconditionally restores false, which becomes incorrect when an owner executing
a values-only parent also participates in a child values-only call. Evaluation
mode and execution-context resources must survive `run_forward_only`'s EvalState
replacement. Freeze process-global kernel registration and packet-math settings
before dispatch; concurrent mutation of them is outside the execution contract.
Treat seeded reverse as consuming a successful forward exactly once: retained
loop backward already releases its saved state. A second reverse needs a fresh
forward, and values-only forward does not authorize reverse.

Deterministic merging forbids atomic floating-point additions and per-worker
partial totals whose grouping depends on scheduling. With K chunks and S active
shared elements, retained chunk adjoints can cost O(K*S), plus child values and
scratch. Report those bytes. A fixed bounded batch schedule can reduce temporary
gradient storage later; recomputation and sparse gathers are separate hypotheses.
Preallocation removes allocator overhead, not the arithmetic or live-storage
cost of these shared contributions.

## Errors, cancellation, and fallback

Each task catches exceptions into a chunk-indexed `exception_ptr`. Drain/join all
dispatched jobs before publishing success, rethrowing, or destroying state. If
multiple chunks fail, select the lowest chunk index deterministically. This is
the threaded error policy, not a promise to match whole-slice exception wording
for every partition. Never retry serially after a runtime domain error.

On failure mark all retained forward state invalid. A later evaluation clears
errors and reconstructs valid state; it cannot run reverse on partial results.
On cancellation stop dispatching new jobs, drain running jobs, invalidate the
evaluation, and return through the existing caller error/interrupt path. Public
R/Python callbacks stay on the calling thread. Scheduler waits must support
caller-side polling for a single long reduction; existing between-transition
polling alone may be insufficient. Partial thread-creation failure joins every
successfully started thread before propagation.

Fallback is selected during lowering/preparation, before executing callback
effects. Record reasons: disabled, nonnative/non-TLS build, effectful callback,
unsupported child lowering, dynamic geometry/grainsize, runtime-region context,
or explicit preparation resource limit. Do not infer that an unsupported parallel
callback is an unsupported Stan model. Large static chunk counts need a declared
memory/preparation budget or a batching design, not silent oversized chunks.

## Alternatives and first experiment

1. **Selected: native child graphs.** Keeps native density kernels and existing
   optimizations. Costs are duplicated plans, packing, saved state, dispatch,
   and serial derivative reduction. Likely attractive for expensive callbacks;
   currently unmeasured.
2. **Interpreter or register-program callback per task.** A reusable callback
   avoids per-chunk graph specialization and can support more runtime shapes.
   It may reintroduce interpretation and nested-tape replay costs. A small
   identical-partition comparison against child graphs should decide whether
   lower preparation/memory outweighs slower gradients. Defer implementation
   until the child-graph prototype measures those costs.
3. **Parallelize vectorized native density kernels directly.** Avoids callback
   binding and could help ordinary models without reduce_sum, but covers fewer
   callback structures and changes shared kernels. Keep as a counterproposal if
   packing dominates or reduce_sum adds little over current vectorization.
4. **Use real TBB as scheduler.** Mature task scheduling, but requires packaging
   and symbol/link changes across shared/static/test builds. It does not solve
   erased reduction boundaries or parent/worker AD ownership. Revisit if pool
   composition becomes the dominant engineering cost.

First implementation experiment would be a C++-only standalone harness: lower
four rewritten callback calls with fixed bounds in isolated trial contexts,
using the retained import contracts and ordinary child Executors. No new opcode,
parent lowering commit, or public API is necessary. For a pure direct-target
likelihood, use existing child gradient calls, native threads with explicit AD
stacks, and a fixed-order owner merge. Compare identical partitions sequentially
and concurrently, and the current whole-slice native model. Include active sliced
and aliased shared inputs. Measure first-use thread creation separately from warm
gradients on persistent harness threads; include all dispatch/packing/merge work
in warm totals. Stop with a numerical report, phase timings, retained bytes, and
a callback-representation decision. A winning unit-seed prototype establishes
headroom for that case, not correctness or performance of arbitrary scalar uses.
Construct child Executors and worker resources outside the repeated gradient
loop. Report first-use setup/growth separately from warm measurements. Count
allocations/deallocations during evaluation separately for the reduction
framework and callback internals, and verify pointer validity during capacity
growth and repeated gradients. Compare reuse and reservation strategies on time
and peak memory; zero allocations is not an acceptance gate. Preallocation-only
versus shared-storage-view binding are separate experiments, so dispatch gains
are not confused with fewer copies or allocations.

Before assuming O(K*N) replication for the brms canary, inspect child fills and
imports: fixed-bound data slices can fold during UDF binding, leaving only the
relevant data slice plus small active imports. Keep a separate large shared
parameter-vector stress case, where that optimization does not remove the cost.

## Evaluator and integration gates

Correctness compares (a) existing whole-slice serial behavior, (b) the exact same
chunk plan on one worker, (c) that plan on 2/4/8 workers, and (d) a matched threaded
CmdStan build. Require bitwise reproducibility for (b)/(c), including repeated
runs and cloned executors. Disabled/default tests retain existing exact parity.
Partitioned versus whole-slice and external results use a predeclared numerical
policy: provisionally `abs(error) <= 1e-10 + 1e-10*abs(reference)` for ordinary
finite fixture lp/gradient values, plus ULP and absolute-error reports. This is
an experimental threshold, not an established project-wide allowance. Stress
cancellation/ill-conditioned sums separately; agree justified bounds before
integration. Check nonfinite classification, signed zero where observable, and
exception behavior explicitly. Do not promise identical NUTS trajectories.

Fixtures cover empty/single/tail chunks, invalid/huge grainsize, nonzero absolute
bounds, nested arrays/matrices, scalar and container shared inputs, active sliced
inputs, overlapping/repeated aliases, normalized/propto densities, multiple
reductions, parameter branches, zero/nonfinite reverse seeds, overwrites after
forward, values-only/gradient alternation, transitive effects, nested reductions,
dynamic-context refusal, worker errors and recovery, cancellation, concurrent
chains/callers, and STANLI_THREADS=OFF. Mechanically compare full parent graph and
compiler state after speculative refusal against disabled lowering.

Performance inputs: synthetic cheap and expensive likelihoods at N=1e3/1e4/1e5,
small and large shared-parameter vectors, and the existing
`tests/brms/s2_threading.stan` as a structural canary with a valid matched dataset.
Record build/CPU identity, inputs and parameter points, preparation, first
gradient, warm forward/reverse/merge, scheduling/packing, RSS, and retained bytes.
Use warmup and at least 10 counterbalanced samples with medians and dispersion.
Provisional target: >=2x warm-gradient speedup at four workers on an expensive
representative likelihood, with no statistically resolved slowdown of default
serial and unrelated canaries. This target is proposed, not a user requirement
or an observed result. Measure full inference throughput and ESS/sec separately.

Before integration run focused reduce_sum, lowering, scan, adjoint, program,
compiler, multichain, C ABI, Python/R, and relevant higher-order suites; then the
full configured suite and sanitizer concurrency/lifetime checks. Clean rebuild
after internal ABI changes. Deliver capability/fallback documentation, effective
thread and partition diagnostics, and reproducible commands/raw evidence.

## Review resolution

Fable reviewed the original draft against source using `claude --model fable`
(reported model `claude-fable-5-1`). All five must-fix findings are incorporated:
early retained-region fallback and explicit child vocabulary; EvalState/team and
Executor-entry plumbing; captured values-only dispatch; graph-pass and fold-time
barriers; and retained-loop import coalescing. Its main simplification, reusing
existing retained infrastructure, is adopted. The eager-gradient suggestion is
accepted for the unit-seed feasibility experiment and conditional future
specialization, not for general use. These revisions have been checked against
the cited source but have not received a second Fable review. Production
implementation and integration validation remain outstanding; the linked
feasibility report records the completed unit-seed experiment.


## Current base and import experiment, 2026-09-18

The working tree is now based on fetched `origin/main` at `504e8d80`, replacing
the original `be0a0c8d` base. Current Executor already separates mutable values
from immutable data, shares data across clones, and detaches before writable
data access. Reuse that ownership mechanism; do not introduce a second one.
Per-child parameter ranges and exclusive/shared gradient destinations remain
separate questions from sharing immutable clone data.

The [compact-import experiment](../plans/2026-09-18-native-reduce-sum-imports.md)
records both historical and current-base evidence. The new-base four-worker
active-slice case reduces warm gradient time from 447 to 308 us and child arenas
from 17.5 to 9.9 MiB while retaining bitwise agreement with the same serial
partition. This supports compact imports and exclusive gradient publication;
it does not establish one physically partitioned arena, general seeded reverse,
or production threading. Runtime integration remains outstanding.
