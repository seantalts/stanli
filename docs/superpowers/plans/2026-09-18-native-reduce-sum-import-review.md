# Fable review: compact reduce_sum imports

Reviewer: `claude-fable-5-1`, via read-only Claude CLI (Read/Grep/Glob only).
Base: fetched `origin/main` at `504e8d80ca05ef1961084ac904c557c698552823`.
Scope: isolated import planner, worker publication, and current Executor / fixed
slice kernel contracts. No production parallel lowering is implied.

Verdict: no blockers. One low-severity finding was addressed after review:
Child now checks the original graph's parameter total against the parent before
compaction, including an upper-bound check while adding lengths. The remaining
post-compaction check verifies the local executor layout. The author rebuilt both configurations and reran all 49 correctness cases,
ThreadSanitizer and stress checks, timings, and regression tests successfully.
Shorter and longer child-layout mutations both refuse before evaluation. This
fix has not itself received a second Fable review.

Two qualifications to the verbatim response: candidate preparation can allocate
multiple buffers after validation, but all throwing work still precedes the
noexcept publishing moves. Also, the statement about all timings using the old
base reflects the review's start state: fresh current-base timings and tests
completed while the review was in progress, and are recorded separately in the
[implementation report](2026-09-18-native-reduce-sum-imports.md).

## Reviewer response (verbatim)

**Review verdict: no blockers found** for the probe on base 504e8d80. Findings below, then verified properties, validation limits, and production-only concerns.

**Findings**

- **Low, `tools/bench_reduce_sum.cpp:251`.** The layout check `n_params() == imported` is tautological once compaction succeeds, since both sides are sums of hull lengths. The check that the child's original parameter total equals the parent size is lost. `compact_imports` computes that total at `tools/reduce_sum_imports.hpp:30-38` but discards it. A shorter child layout would go undetected, and a longer one surfaces only as `out_of_range` from `readers.at()` at `reduce_sum_imports.hpp:133`. Unreachable in this fixture. Fix: assert the pre-compaction total against `nparams` before calling `compact_imports`.

**Verified correct**

- **Transactional refusal.** Every `return false` in `compact_imports` precedes the copy at `reduce_sum_imports.hpp:90`. The copy is the only throwing step after validation and does not touch caller state. The three publishing moves at lines 120-122 are noexcept. The refusal test at `bench_reduce_sum.cpp:523-546` covers this.
- **Immediate ownership across copies.** Rebased offsets go into the candidate's own pool at line 111. The Graph copy constructor at `runtime/include/stanli/graph.hpp:46-68` rebinds pool-owned pointers and shares finalized views. `compact_idata()` at `runtime/src/executor.cpp:302` early-returns on a finalized graph, so appended entries stay mutable and rebind on clone. The clone test at `bench_reduce_sum.cpp:671-679` exercises this.
- **Hull soundness.** Only slots read exclusively by exact-shape fixed `OP_INDEX`/`OP_SLICE` shrink. Written slots, the result slot, and any other consumer force the full range. Ops carrying opaque payloads or kernel state, including `OP_LOOP` and `OP_ISLAND`, are refused, so no slot is referenced outside `in[]`. The fixed kernels at `runtime/kernels/elementwise.cpp:216-264` read and scatter only within `[start, start+out.len)`, which lies inside the hull after rebasing.
- **Gradient publication races.** Exclusive coordinates have exactly one reader by construction. `merged.grad` is zeroed at `bench_reduce_sum.cpp:343` before `team->run()`, and the mutex in `Team::run` orders that clear before any worker touches it. Nothing reads `merged` until `remaining_ == 0` is observed under the same lock. A refused child imports the full range, forcing every coordinate shared. `Team` drains all jobs before rethrowing the lowest-index error, and `errors_` is only reset after the previous generation fully completed.
- **Executor sharing.** Each child compiles its own model, so shared immutable data is per-child. The values-only flag at `executor.cpp:43` is thread-local.

**Validation limits**

- Bitwise equality between compact and full modes assumes non-owner children yield exactly `+0.0` adjoints from the memset. Two `-0.0` contributions would differ bitwise. Not reachable here.
- With `active=1`, every active coordinate is exclusive and the prefix is shared by all. Partial overlap on the real `Team` is exercised only by `check_scatter_publication` with two jobs and at most two threads.
- Any fill whose size differs from its slot length refuses the whole chunk at `reduce_sum_imports.hpp:85-87`. Watch `compact_accepted_chunks` in the new-base reports.
- All stored timing reports describe the old base, as noted.

**Production concerns, not this experiment**

- Shrinking `Slot.len` on parameter slots breaks `unc_params` and `param_names` alignment for write_array and transforms. Production must compact the callback subgraph's imports, not model parameter slots.
- Parent offsets assume the child's parameter slot order and lengths mirror the parent's.
- Worker publication into a parent adjoint arena is safe only if the reduce_sum backward is one joined step in the serial reverse sweep.
