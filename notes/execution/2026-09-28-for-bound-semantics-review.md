# For-bound semantics: Fable review

Review requested through `claude -p --tools "" --output-format text`, with the plan and patch supplied on stdin. This is a source review, not reviewer-run validation.

## Dispositions

- MIR stores both if arms, nested loop bodies, and indexed assignments in the fields traversed by `assigned_names`. Added else-arm and nested-while mutations to the independent oracle fixture.
- Existing island live-outs erase folded integer bindings. Added post-loop uses as an index and another loop bound to test this boundary.
- Register assignments write the reified register in place. Mutable-upper callbacks and growing/shrinking source loops are exercised.
- Register PRINT already executes only for double forward execution; var replay suppresses it. REJECT stops the forward before backward can run. The island source comment claiming otherwise is stale; no new replay behavior is introduced.
- Structured candidate failures are caught transactionally. Ordinary runtime-loop routing now tries a register island after structured refusal; nested unsafe upper bounds cannot silently fall through to captured-bound execution.
- Registered numeric builtins are pure except the explicit RNG policy. Unknown calls remain conservative. The upper-bound capture proof is independent of successful constant folding, and target reads fail that proof.
- Effect analysis follows user-function call graphs; it does not mark every UDF effectful. Known container shape queries are independent of element mutations, preserving existing unrolling for ordinary array loops.
- Effectful lower bounds route directly to the register loop. The source RNG fixture exercises ordered lower and upper evaluations, including an empty loop.
- The region integer-contract check currently copies the statement tree when admission is needed. This is preparation work; removing that temporary belongs with the upcoming integer-contract changes.

## Review text

The plan's direction is right: the interpreter change is correct as written, and routing mutable or effectful bounds to the register loop with head-and-backedge reevaluation matches the pinned stanc C++. The risks are in the admission predicates and in what happens around the new island. Findings in priority order.

**1. `assigned_names` only recurses into `s.body` in both copies.** If `If` or `While` keep the else arm or condition body in another field, assignments there are invisible. The bound is then judged invariant, and the graph or structured path captures it, reproducing the original bug. Verify the traversal covers every child field, plus compound and indexed assignment kinds. Add fixture cases that mutate the bound only in an `else` arm and only inside a nested `while`.

**2. Stale integer folding after the island.** Routing a For through `lower_runtime_ifelse` means the body now mutates outer integers at runtime. Every name in `written`, and the loop variable, must be evicted from the lowering's folded integer environment after the island, or later statements fold pre-loop values. Add a fixture case that uses the shrunk bound after the loop as another loop bound and as an index.

**3. The head read must see in-place writes.** The upper expression is emitted once at the head and re-executed on the backedge. That is correct only if body assignments to outer scalars write into the same register rather than rebinding the name. The "aliased Range" comment suggests in-place writes. The "repeated integer bindings" bail suggests some bindings are not. Confirm for scalar int locals written with compound assignment inside the loop.

**4. Effect replay regression in the model block.** The removed "invariant, effect-free" bail also kept effectful user function bounds out of register programs. The island comment says those programs replay during reverse mode. The new conformance test proves the forward print count, not the reverse-mode count. RNG bounds are safe because they only occur in transformed data and generated quantities. A print or reject inside a user function used as a bound in the model block is not. Keep a bail for an effectful upper outside write_array until the execute-once path exists, or show the replay does not happen.

**5. The structured-loop `fail` must be recoverable.** If it is a hard compile error rather than a region rejection, any counted outer loop containing an inner loop with a mutable bound breaks the model instead of falling back to unrolling. Also confirm where it lands when the outer loop is runtime-counted and has no unrolled fallback.

**6. Elementwise and Product widening is load-bearing on the folder.** It is safe today only because the unrolled path also requires both bounds to fold, and vectorized RNG calls cannot fold. The predicate is named as an effect proof, so a future caller without that gate admits RNG bounds. Prefer an explicit effect or RNG bit on the builtin spec over shape policy. Also check whether `target()` is admissible while the body adds to target.

**7. Performance surface.** If `expr_effectful` treats every user function call as effectful, a pure user function bound now leaves the graph path. That is a regression on ordinary models. Confirm how user functions are classified, and compare engine-selection counts on the corpus before and after. Bounds that reference body-assigned names were wrong before, so routing them costs nothing.

**8. Lower-bound effects are unchecked.** Only the upper bound is inspected. An RNG in the lower bound on the graph path reaches `eval_int` before any routing decision. Add "RNG in lower" to the fixture and assert it draws once, before the first upper test.

**Minor.**
- The integer contract check copies the whole statement tree per runtime loop compile. Pass a pointer or span.
- `hi` is now unused on the runtime branch and may warn as maybe-uninitialized.
- The flipped `f_runtime_for_mutates_bound` expectation should assert the C++ iteration count, not just admission.

**Fixture coverage.** Include a shrinking bound, a growing bound with a cap, `continue` to prove increment-then-test, an empty range with an RNG bound drawing exactly once, mutation in else and while arms, post-loop use of the mutated variable, and an inner bound depending on an outer-mutated variable. Place cases in transformed data, transformed parameters, model, and generated quantities so all four engines are exercised. The unit test only covers the register engine, so run the fixture under the existing engine-forcing flags as well. Confirm the reference file was recorded from CmdStan, not from Stanli.

The later plan items on integer operations, callback layouts, and dynamic selectors are not in this diff and need no comment yet.
