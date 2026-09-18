All three emitters attach a `BoundCheckSpec` and nothing else, so the whitelist is closed. Nothing further is needed.

**The three blockers are resolved.** No remaining concrete blocker in these changes.

- **Blocker 1, bounded specialization.** `runtime/src/lower.cpp:921` copies the parent's compile options into the trial, and the region-trial constructor at line 42 does the same for children. The new bounded fixture is a real candidate: a `while` statement makes `region_runtime_control` return true, and its `k <= N` guard passes `bounded_while_guard`. So the one-retained-opcode assertion in `native_review_regressions` would have failed before the fix and is not vacuous.

- **Blocker 2, nested reductions.** The whitelist at `runtime/include/stanli/detail/input_ranges.hpp:54` covers exactly the three check opcodes. Every emitter of those opcodes in `runtime/src` attaches only a `BoundCheckSpec`, which is a name plus two booleans with no slot ids. Check ops still set their output as written and read their inputs over the full range, so no trimming applies to them. `Graph` copies share the payload pool, so the pointer survives the candidate copy. The shapes fixture now yields two retained sites, with the inner reduction serial inside the child by design, matching the strengthened test.

- **Blocker 3, refusal visibility.** The refuse lambda in `try_lower_parallel_reduce_sum` records a deduplicated, callback-named reason for every refusal path, including the planner's named opcode reason. Only `CompileError` is caught now, so lowering bugs propagate. The runner prints each reason to stderr when threads per chain exceed one. The corner test asserts a non-empty reason plus graph and fill equality against the serial compile on every refused case.

The oracles in the new regression test check out against the fixtures: the discrete model has one normalized and one propto Poisson site, matching the expected expression, and the bounded model's while loop unrolls to seven additions of `b`.

**One non-blocking nit.** The empty-slice note at `runtime/src/lower_higher_order.cpp:799` is pushed unconditionally, without the dedupe or callback-name prefix the refuse lambda uses. Two empty-slice sites would produce duplicate notes. It does not affect correctness.
