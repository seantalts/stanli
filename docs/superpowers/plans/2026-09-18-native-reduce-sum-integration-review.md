Review complete. Findings below, ordered by priority.

## Verdict

I found no wrong-answer defect in the kernel, scheduler, or import planner as written. The scheduler's synchronization is sound, the values-only capture is correct, child binding pointers are stable, and the parameter-offset layout in the reduction state matches the executor's bind order and reverse memcpy. The problems are silent no-ops, silent refusals, and coverage gaps. Three of them should block merge.

## Blockers

**1. The opt-in silently no-ops under bounded specialization.** `try_bounded_specialization` at `runtime/src/lower.cpp:919` constructs a fresh trial Lowering and never copies `compile_options`, so the trial sees the default of one thread. That path runs by default when the environment variable is unset and the model is a candidate. The candidate predicate at `runtime/src/lower.cpp:866` only refuses direct user-defined calls and range indexing in the model block. A model whose only UDF use is through reduce_sum passes it. `tests/fixtures/reduce_sum_shapes.stan` looks like exactly such a model, which would mean `shapes_and_overloads(true)` exercises nothing today. Fix is one assignment after line 926. The proof is an `OP_REDUCE_SUM` count assertion in that test, which should fail before the fix.

**2. Nested reductions are refused, contradicting the spec and the design doc.** Inside a child, `lower_reduce_sum` still emits the grainsize check with a `BoundCheckSpec` payload at `runtime/src/lower_higher_order.cpp:755`. `compact_imports` refuses any op with `udata` at `runtime/include/stanli/detail/input_ranges.hpp:44`. So the third reduction in the shapes fixture goes serial silently. Either whitelist payload types that name no slots, or document nested reductions as a refusal reason. Payload sources in `runtime/src` are limited to checks, ODE and solver specs, islands, and loops, so a whitelist for the check spec is small and safe.

**3. Refusal is invisible and swallows internal bugs.** `try_lower_parallel_reduce_sum` records no reason anywhere. The catch at `runtime/src/lower_higher_order.cpp:665` turns any `std::logic_error`, including `out_of_range` and `length_error` from a lowering bug, into silent serial execution. `tools/stanli_run.cpp:162` only prints a count. Narrow the catch to `CompileError` plus an explicit refusal type, and surface reasons the way `interpreter_fallbacks` does. Silent-serial cases that need documenting: the element cutoff, `reduce_sum_static` exceeding `max_chunks`, non-threaded builds, payload and dynamic-op refusals, and retained-region context.

## Test gaps, ordered by risk

- **Integer-array sliced argument** with a discrete density. This is the canonical brms pattern and only reaches the serial path in the shapes fixture because grainsize 100 yields one chunk. The `DeclView` construction at `runtime/src/lower_higher_order.cpp:618` drops the integer-array flag, which matches the serial binder at `runtime/src/lower_funapp.cpp:498`, so it is plausibly fine. It is untested.
- **`reduce_sum_static` on the parallel path.** No fixture reaches the fixed-partition branch with more than one chunk.
- **More than six live inputs.** The `OP_CONCAT2` packing at `runtime/src/lower_higher_order.cpp:679` and its offset remap are unexercised.
- **Child domain error and recovery** through the integrated kernel, not just the probe. Confirm `ready` stays false and the next forward recovers.
- **Refusal equivalence.** Compare the parent graph after a refused parallel compile against the serial compile. `argument_evaluation(true)` already produces refusals and is the natural spot.
- **Two reductions in one graph**, and effectful argument expressions with an accepted pure callback.
- **ThreadSanitizer on `test_reduce_sum` itself.** The imports document records TSAN only for the standalone probe.

## Verified sound

- **Scheduler.** Job state is written before the lock scope in `run`, workers read after acquiring the mutex, and `remaining_` is waited to zero before return. Errors drain before the lowest-index rethrow. Generation counting cannot lose a wakeup.
- **Values-only mode.** The owner captures the thread-local flag and workers select the child entry point from it. The guard at `runtime/src/executor.cpp:679` now restores its previous value.
- **Binding stability.** `value_ptr` detaches shared data once at construction, and child executors are never cloned, so bound input pointers do not move.
- **Parent passes.** Effect-barrier membership covers constfold, CSE, partition, and reroll. Store forwarding refuses the reduction as a reader. In-place rewrites only target store ops and never a slot without a producer, so import slots are safe. Clones rebuild kernel state through `bind_`.
- **Kernel thread safety.** No file-scope mutable statics in kernel sources under `runtime/src` or `runtime/include` beyond a thread-local recorder sink and the read-only packet flag.

## Refinements, not blockers

- **Idle teams.** `tools/stanli_run.cpp:184` builds one team per chain, not per active chain. Eight chains on four sampler threads with four threads per chain create sixteen idle workers.
- **Constant folding off in children** at `runtime/src/lower_higher_order.cpp:649` is safe but regresses data-only callback arithmetic relative to serial. A targeted follow-up would treat import slots as opaque inputs to constfold rather than disabling the pass.
- **Compile cost.** `fork_region_trial` deep-copies the transformed-data environment once per chunk.
- **Conservatism.** The reduction copies inputs in forward, so marking it a value reader in `make_inplace_updates` and refusing the rename at `runtime/src/inplace.cpp:386` are stricter than the operand-indexed binding requires.
- **Load balance.** Dynamic partition uses exactly `threads` chunks, so a straggler holds the owner. The spec suggested a small multiple.

## Genericity assessment

The design is structural, with no name matching, and the import contract is operand plus offset as the spec required. The effective surface is narrower than the spec claims: pure callbacks with fixed shapes, no payload-carrying ops, no dynamic geometry, no retained loops, and no nested reductions. That is a reasonable v1 boundary once it is documented and reported, which blocker 3 addresses.

---

Lead-agent follow-up: all three blockers were addressed. The compiler trial
copies options; only immutable BoundCheckSpec payloads are admitted for nested
validation; structural/CompileError refusal reasons are exposed, while internal
logic errors propagate. The targeted gaps are now covered in test_reduce_sum,
including native integer Poisson, static chunks, packed operands, nonlinear
seeds, argument effects, error/recovery, graph/fill refusal equality, concurrent
clones, and NUTS. 254 tests and the native TSAN suite pass. The [bounded Fable follow-up](2026-09-18-native-reduce-sum-integration-review-followup.md)
confirms all three blockers are resolved.
