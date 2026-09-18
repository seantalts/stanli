# Fable review: native reduce_sum parallelism

Date: 2026-09-17. Reviewer: Claude Fable (`claude-fable-5-1`, as reported by
Claude CLI). Reviewed source: `be0a0c8d4882afca3e1d1de0083267df10f8fb3d`.
The review was read-only, with Read/Grep/Glob tools and no builds or implementation.
The findings below refer to the original draft; its line numbers predate revision.

[Revised design and finding resolutions](../specs/2026-09-17-native-reduce-sum-parallelism.md).
The author verified the cited retained-loop seams and incorporated all five
must-fix findings. No second review or performance validation is implied.

## Reviewer response (verbatim)

Review complete. I read the spec and the cited runtime seams, plus the retained-loop machinery the spec does not cite. No files were written and nothing was built or run.

## Headline finding: the boundary already exists

The spec treats child graphs, input packing, transactional lowering, per-executor mutable state, and seeded reverse as new machinery. All five exist today for the retained loop op, and the spec never names it:

- Child graph plus constant fills in an immutable payload: `runtime/include/stanli/structured_loop.hpp:97-99`.
- Imports carrying `active`, `data_only`, and a packed offset: `runtime/include/stanli/structured_loop.hpp:89-95`.
- More than six inputs packed through concatenation with per-import offsets: `runtime/src/lower_structured_loop.inc:2107-2117` and `2173-2184`.
- Transactional trial lowering with commit only on success: `runtime/src/lower_structured_loop.inc:1995-2171`.
- Fresh kernel state per bound executor, including copies: `runtime/src/executor.cpp:380-389` and `486-489`.
- Seeded reverse from the incoming adjoint vector into the parent's input adjoints: `runtime/src/structured_loop.cpp:2140-2164` and `1533-1540`.

The spec should say it builds on this seam, or state why not. That turns the feasibility question from "unknown" into "dispatch K existing child bodies to threads and merge."

## Must-fix

1. **Retained-region interaction is wrong as written.** Loop preparation refuses any stateful body kernel and any nested island or loop op at `runtime/src/structured_loop.cpp:133-148`, and the trial fails on islands at `lower_structured_loop.inc:2118-2120`. A stateful reduce op emitted while `region_current` is set would refuse the whole enclosing region, not just the reduction. Lowering must check `region_current` and take the serial rewrite before emitting anything. The spec must also state which body vocabulary a child admits, since islands and nested loops are refused by one host and not the other.

2. **No path from a kernel to the thread team.** Kernels see only `KernelCtx` at `runtime/include/stanli/kernel_types.hpp:73-93`. Its evaluation state holds one RNG pointer, and its kernel state is per op per executor. The gradient entry point at `runtime/src/executor.cpp:650` takes no evaluation state. A team hung off kernel state is per op, and pooled clones from `executor_pool.hpp` would each spawn their own. The "one team per active chain" budget on spec lines 127-134 is unenforceable until the spec names the plumbing: a team pointer on the evaluation state plus a gradient overload that accepts it, or an executor member the sampler sets.

3. **Values-only propagation needs a stated protocol.** The values-only flag is thread-local at `runtime/src/executor.cpp:43`. The owner must read it before dispatch and workers must call the value-only forward on the child. Spec line 42 names the trap but not the rule.

4. **Graph passes and fold-time evaluation are not addressed.** Only the in-place pass treats the loop op as a barrier, at `runtime/src/inplace.cpp:385`. Compile-time evaluation instantiates stateful kernels at `runtime/src/higher_order_eval.cpp:131-136`, so the new op must remain structural on that path. The spec should list every pass that must treat the new op like the loop op.

5. **Aliased inputs need the import-merge rule, not a new one.** The loop op already coalesces repeated parent slots into one import at `lower_structured_loop.inc:2013-2021`. Adopt that rule and its adjoint accumulation instead of inventing "preserve distinct formal derivatives then add."

## Optional refinements

- **Reconsider the ban on eager per-chunk gradients** on spec lines 156-159. Every task calling the existing child gradient gives one fork/join per parent gradient instead of two, no retained child forward state, and no invalidate-partial-reverse protocol. When the reduction feeds `target +=`, the incoming seed is exactly one, so results are bitwise identical to seeded reverse. Offer a fused mode for a unit seed and seeded reverse otherwise.
- **Shared-data replication may be small for the canary.** The threading fixture at `tests/brms/s2_threading.stan:18-21` slices data arguments with the bounds. With literal bounds, data observations bind into the compile-time environment at `runtime/src/lower_funapp.cpp:491-494` and pure data subtrees fold at `runtime/src/lower_expr.cpp:1668`. Verify that the sliced data folds to a constant fill per child. If so, children import only the three parameters and the replication concern on spec lines 90-91 collapses. The real retained-adjoint risk is a large shared parameter vector, which the spec already names.
- **Thread-local scratch is per worker and safe across workers.** Islands use a non-reentrant thread-local register file at `runtime/include/stanli/island.hpp:129-134`, and the ODE workspace is thread-local at `runtime/kernels/ode.cpp:236`. One chunk per worker at a time keeps these correct. The nested-reduction rule on spec line 73 must remain, since an island inside a chunk inside an island would reenter.
- **The tolerance policy is fine as an experiment.** A relative bound near ten to the minus ten covers regrouped sums at the stated sizes.

## Prototype verdict

A bounded prototype is justified and can be cheaper than the spec's version. Hand-build four child graphs by lowering the rewritten call with fixed bounds through a trial lowering, run each child's existing gradient on plain threads that construct a chainable stack at entry as in `runtime/src/executor_pool.cpp:22`, and merge in fixed chunk order. No new opcode and no lowering commit. That directly answers the headroom question on spec lines 17-18 and decides the fused-versus-seeded question before any API work.
