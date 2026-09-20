# Fable 5.1 review of post-392 execution plan

Reviewed base: `1449c018406e961bc5a3b103740bd122b113fd29`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

**Verdict: revise, then implement.** The plan's direction is sound and its kernel claims check out against the code. It is missing an attribution method that can see inside the retained loop, a per-family eligibility list, a stop criterion, and one gate that is weaker than the plan assumes. None of these needs a new design exercise.

## Issues, ranked by severity

**1. Step 1 cannot use the opcode profiler.** The profiler at `runtime/src/executor.cpp:650` and `:723` times top-level ops only, and `structured_loop.cpp` has no profile hooks. The whole ctsem loop will appear as one OP_LOOP line. Step 1 must instead stack-sample the warmed window that the timed benchmark driver already provides, and pair that with an opcode-plus-variant census of the ctsem body. The fixture lives in the ignored artifact directory, so no census exists in this worktree. Add a numeric no-go rule: if solve and matrix-exp kernels together fall below a stated share of warmed samples, the kernel track stops and the deferral is recorded.

**2. The forward "bitwise" gate is not enforced today.** The pullback test's value check accepts a relative tolerance unless an exact flag is passed (`tests/test_native_matrix_pullbacks.cpp:19`), and the solve value check at line 350 does not pass it. A direct-double forward that claims to reproduce the active overload's arithmetic must be gated exactly over the family sweep, or the parity proof the plan requires is unproven. Make that a test change inside the implementation.

**3. Right-side families have no rev overloads at the pin.** Reads of the rev headers for the right plain and right SPD solves failed, and a search of the rev function directory finds no right-side file. The source list in the task brief is stale on this point, so confirm with a directory listing. The consequences differ by family:

- **Right plain** at var resolves to the prim template, which runs LU on a var matrix (`deps/math/stan/math/prim/fun/mdivide_right.hpp:36`). Elementwise var arithmetic is unlikely to match double packet arithmetic bitwise. Keep it on the old path unless the census shows it matters.
- **Right SPD** at var goes through the prim wrapper into a left solve on the transposed dividend (`prim/fun/mdivide_right_spd.hpp:40`). The prim left overload excludes var, so this should land in the rev left overloads, which are LLT on doubles. Verify that resolution, then it is reproducible as a transposed double LLT solve.
- **Right triangular** at var runs the prim triangular template on var scalars (`prim/fun/mdivide_right_tri.hpp:43`). Same caveat as right plain.

**4. Triangular solves have no factor to retain.** The triangular adjoint solves against the retained input directly (`runtime/kernels/matrix_fns.cpp:1025`). Factor retention only applies to plain-left and SPD. Per call, the retained cost would be:

| Family | Retained doubles per call |
| --- | --- |
| Plain left (Householder QR plus coefficients) | n² + n |
| SPD (LLT lower triangle) | n² |
| Triangular | none |

**5. The retention mechanism already exists; name it.** The plan's remark about mutable kernel state is correct, since retained bodies reject it (`runtime/src/structured_loop.cpp:167`). The right tool is the per-call scratch size hook on the kernel. Scratch is allocated per call in the arena (line 1799), collected as a live range at seal (line 2449), encoded into frame values (`runtime/src/structured_frames.inc:109`), and rebound at backward (line 388). The symmetric eigenvalue kernels are the precedent (`matrix_fns.cpp:1348` through `:1409`). Adding scratch to solves does not disturb the reuse-primal pilot, because the solve registrations leave the primal-reads callback null and that pilot already skips such kernels (line 451). The lifetime gate is: a backward must never consume scratch written by a different forward, including value-only alternation. Kernels already skip scratch when the thread-local value-only flag is set (`executor.cpp:43`).

**6. Step 2 conflates two removable costs.** The active forward builds a nested tape and arena allocations (`matrix_fns.cpp:1003`), and the backward repeats the factorization (`:1042`). Stage 2a, a direct double forward with no retention, is independently measurable and is probably the larger saving at small n. Require its paired result before stage 2b is considered.

**7. Matrix-exp corrections.** There is no rev matrix-exp overload either, so the forward is already the prim path. The backward exponentiates a 2n block (`matrix_fns.cpp:243`), roughly eight times the forward's arithmetic, with the Padé degree chosen on the block's norm. A Fréchet algorithm recomputes the forward recurrences anyway, so reusing forward intermediates is an optimization rather than a prerequisite. The plan's cited oracle exists: `tools/matrix_pullback_hp_check.py` runs mpmath at 60 digits. The current gates are 1e-14 scale-relative against the var tape and 1e-6 against finite differences. A different algorithm may not hold 1e-14. The plan must state the allowance it will accept and justify it with the mpmath tool before any model comparison. Also note the 2x2 closed-form branch in the prim exponential: the forward takes it, the 4x4 backward block does not, and a Fréchet path must be consistent there.

**8. Name the guard diagnostic.** The respecialization count is printed under the structured-loop diagnostics environment variable (`structured_loop.cpp:3173`).

## Eligible families and how to preserve the distinctions

Eligible without a scalar-order reimplementation: plain left (Householder QR on values, `deps/math/stan/math/rev/fun/mdivide_left.hpp:45`), SPD left and right (double LLT), and triangular left (double triangular solve). Each direct forward must keep the same check functions in the same order as its overload, return the same empty shape for size zero, and keep the vector-versus-matrix dividend type that the kernel's dividend alias already encodes. The value-only branch stays exactly as it is: variant bit zero plus the thread-local flag select the prim path, and the Hilbert fixture in `tests/test_solve.cpp:145` pins the LU-versus-QR difference. Keep the old nested-tape forward as a test-side oracle, not a production branch.

## Refined order and gates

0. Census plus warmed stack sample. Choose a target or record the deferral. Hours, not days.
1. Direct double forward for the eligible families, exact-gated against the old forward over the family sweep, measured alone in six pairs.
2. Factor retention for plain-left and SPD only if the sample shows backward factorization is material. Report retained bytes against removed work.
3. Matrix-exp Fréchet only if step 0 selects it, with the accuracy allowance decided first.

Every step passes the bitwise model gate at four sizes and three points, the solve fixture, the pullback suite with exact values, the corpus replay, and the six-pair timing protocol before it is kept.


## Lead reconciliation

Accepted the family eligibility, exact-value tests, separate forward/factor
ablations, and named scratch-lifetime gates. The plan already required warmed
stack sampling and existing scratch; the review makes these explicit. Added a
provisional 10% inclusive kernel share for choosing the next experiment.

Qualifications: a doubled matrix dimension does not establish an eightfold
wall-time ratio; Padé selection and sparsity require measurement. No new
matrix-exponential error allowance is accepted here. If selected, that
experiment must justify accuracy against the independent high-precision oracle
before model acceptance. The fixture is present in this worktree's ignored
cache; its new runtime census is still to be measured. Right-SPD forward and
backward use different triangles for accepted nearly symmetric inputs, so
forward-factor reuse is not automatically safe. Unchanged arithmetic is gated
bitwise, including the forward kernel sweep; backward formulas remain intact
in the first stage.
