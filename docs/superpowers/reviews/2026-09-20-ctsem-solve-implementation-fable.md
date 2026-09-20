# Fable 5.1 review of post-392 solve implementation

This review covers the direct-forward implementation and the experimental QR
retention stage. Subsequent performance gates excluded both retention
variants. The production diff retains direct forwards and the exact forward
oracle; the reviewed retention code and `test_solve_reuse.cpp` are archived
under `.cache/ctsem-post-392/reuse-matrix/` for further research. Findings and
their numerical/lifetime validation below remain evidence about that prototype,
not a claim that retention is enabled in the final patch.

Reviewed base: `1449c018406e961bc5a3b103740bd122b113fd29`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

Both confirmed: Eigen 5.0.1's Householder block threshold is 48, so n=55 with k=3 in the reuse test hits the blocked path, and the loop diagnostics already print a `respecialized=` count.

**Verdict: no blocking correctness findings.** The direct forwards match the pinned explicit `Matrix<var>` overloads, the QR retention is consistently sized and bound by every scratch producer, and the new tests do exercise the exact overloads and lifetimes. Ranked findings follow.

**1. Unmeasured: re-recording after a guard failure is not directly exercised.** The synthetic recurrence in `tests/test_solve_reuse.cpp` has no data-dependent control flow, so the reset-and-respecialize path at `runtime/src/structured_loop.cpp:3200` never fires. Changing parameter points changes values, not structure. The mechanism is generic (fresh arena, fresh pointer capture at line 1743, no scratch shared across recordings), so this is a coverage gap rather than a suspected bug. Cheap closure: the diagnostics the test already captures print a respecialized count. Assert it, or add a point-dependent branch so it becomes nonzero.

**2. Low risk: activity 0 legacy encoding backward with scratch is untested.** The forwards test covers activity 0; the pullback test iterates only activities 1 to 3; the reuse test uses activity 3. The backward default branch at `runtime/kernels/matrix_fns.cpp:1233` is shared with activity 3, and the scratch hook keys on bit 0 only, so consistency holds by construction.

**3. Optional cleanup: triangular check name.** The kernel checks under the name "mdivide_left_tri" while the pinned entry point `mdivide_left_tri_low` checks first under its own name. That path is unreachable because the kernel builds both operands from idata, so square and multiplicable can never fail. The message claim technically holds only because no fixture can reach it.

**4. Cross-platform floating point: reasoned and verified on darwin only.** The saved path applies the same HouseholderSequence algorithm with identical length and shift as the pinned `householderQ()` expression. Eigen 5.0.1's gemv uses unaligned loads with index-based blocking, so scratch alignment, which differs from a heap matrix, does not change reduction grouping. The reuse test at n=55, k=1 places scratch at an 8-byte-odd offset and is bitwise. x86 AVX/FMA runs of the bitwise gates remain a validation requirement, not a blocker.

**5. Shared invariant, not new.** Backward trusts any non-null scratch when the active bit is set and n is positive. I confirmed every binder sizes from the same Op and variant: the executor, structured loop preparation, island lowering, program kernel calls, and the interpreter. Backward after value-only is refused by the executor's fresh-forward guard and the reduce_sum ready flag. The var replay path copies the value arena including scratch after forward, so its callback reads the right factor. This is the same pattern every scratch-stashing density kernel already relies on.

**Audited and passing**, with locations:

- Non-null zero-sized scratch: the executor's one-past-end pointer is never dereferenced, frames encode length 0 as null so backward recomputes, and transient workspace is sized with the kernel scratch.
- Zero dimensions: n=0 returns at `matrix_fns.cpp:1034` before the scratch write at 1036, and backward passes null when n is 0. k=0 with positive n is sized and written.
- Reuse-primal is excluded because it requires zero kernel scratch (`structured_loop.cpp:444`).
- Arena block layout matches between retained forward (line 1802) and record backward (line 2215), stream pointers, and frame values.
- Check order and dividend evaluation style match the pinned headers for plain, SPD, triangular, and right SPD, including the right-SPD wrapper re-checking under the left name.
- Dynamic lengths never apply to solve ops, so the static scratch size is authoritative.
- Ownership: copies rebuild scratch through bind, and the map-backed HouseholderSequence never outlives the adjoint call.
- Tests exercise the exact overloads: the forwards test uses exact activity scalar types and Eigen shapes with bitwise or NaN-class comparison, including n=0, k=0, and activity 0. The reuse test covers the blocked Householder path, odd scratch offsets, forced frames, clones, and value-only alternation.

**Remaining validation before approving the retention stage:** paired QR-retention timings, the per-trip retained byte cost of n squared plus n doubles per active plain-left call, the full CTest run, and cross-platform bitwise gates.


## Lead reconciliation and subsequent validation

Accepted both coverage suggestions: the synthetic recurrence now changes a
parameter-controlled branch and asserts two re-specializations, and the exact
cached-versus-recomputed pullback sweep includes legacy activity zero. Both checks now pass, as do the five focused solve/matrix/structured-loop
suites. The frame path returns before printing the stream counter, so its
test asserts completed frame recordings after both branch changes; the stream
test asserts the explicit re-specialization counter.

The triangular wrapper's dimension checks cannot fail for the fixed square
and multiplicable shapes constructed by this kernel ABI. The existing external
shape checks are unchanged; no new reachable exception-message difference was
found. No production change is needed for that observation.

The fresh QR ablation and final base comparison establish the retention-stage
benefit; numerical and full-test results are recorded in the execution plan.
This host verifies Apple arm64. x86 bitwise execution remains for CI; the
reviewer's alignment/reduction-order audit is reasoning, not an x86 test run.
The review prompt understated the external-error figure. The maximum over all
target sizes/points is 3.2102e-13 (4,000 rows, point 0), still below the existing
1e-9 gate. All internal target comparisons are bitwise.


A later retention prototype copies the saved QR and coefficients
into the original MatD/VecD sequence types. The reviewed map-based variant had
small slowdowns on unrelated controls; the original-type variant is neutral
on the four focused six-pair controls and passes all exact tests. Retention,
binding, activity selection and accumulation expressions remain unchanged.
This choice removes the additional map-sequence template instantiations; the
precise contribution of link layout versus shared code generation is not
claimed. Final-model checks at all four sizes and three points are bitwise,
and the 329-model reference gate and full CTest have been rerun for this variant.

The final broad screen and fixed A/A/A-B follow-up subsequently found residual
slowdowns on `lotka_volterra` and `s2_mv_subset`. The direct-only ablation is
neutral on both. Both QR-retention variants were therefore removed from the
production diff, while their review and experiments remain archived. This is
a performance rejection, not a new correctness finding. The selected direct
forward implementation is byte-identical to its independently measured stage.
