# Fable review: normal identity GLM recorder

Read-only claude --model fable --effort medium review.

## Dispositions

- Upstream propagator order verified at pinned normal_id_glm_lpdf.hpp:137: y, x, alpha, beta, sigma. Exact weighted-partial tests independently cover all five edges.
- Row-vector adapter provenance is checked in density_registry.cpp; row vectors have rows=1, and the emitted idata retains those rows. Tests exercise row-vector broadcast with active X.
- Forward errors invalidate Executor reverse readiness before any kernel runs (executor.cpp:696); gradient cannot reverse failed partials. Scoped recorder state unwinds on rejection. Invalid-input tests compare exception text to the preceding implementation.
- No floating-point mismatches were found in the broad exact internal evaluator; full corpus gates pass. This translation unit retains the project floating-point flags, without fast-math.
- Scratch sizing and backward contraction were not changed. Review limitations below remain explicit.

## Review text

Review of the bounded normal_id_glm recorder migration, comparing the new `nid_glm_eval` at `runtime/kernels/matrix_fns.cpp:996-1069` against the committed var-tape version and the recorder in `runtime/include/stanli/recorder.hpp`.

**Verdict.** No confirmed defect on the evidence gathered. The migration preserves the dispatch, scratch layout, propto instantiation and early-return semantics. Four questions remain, one of which is load-bearing and could not be verified within the read budget.

**Confirmed parity**

- **Scratch layout.** Sink registration at `matrix_fns.cpp:1003-1010` yields `[y if y_var][X if x_var][alpha][beta][sigma]`. The old copy-out loop produced the same order, and the matrix edge emits in column-major storage order (`recorder.hpp:287-289`), matching the old `Xv(i,j).adj()` loop.
- **Geometry dispatch.** The `one_y`, `one_a`, `one_s`, `row_x` and `pick_x` logic is line-for-line the old logic. Scalar y binds `yv(0)` as a scalar rvar, so upstream's `is_vector<T_y>` branch selection is unchanged.
- **propto.** `is_constant<rvar>` is false exactly as for var, so every `include_summand` instantiation is identical. Under propto with data y and X, the log-sigma term is still kept because sigma is active.
- **Early returns.** `record_probability_call` (`recorder.hpp:137-148`) captures the literal 0 return and zero-fills every registered buffer when `build()` never ran. The old path read zero adjoints from an untouched tape. Under `values_only()` buffers are null and the fill is skipped.
- **Signed zero.** The `0.0 +` loop at `matrix_fns.cpp:1064-1067` reproduces the old accumulate-into-positive-zero adjoint. This holds only without fast-math or no-signed-zeros on this translation unit, which would fold the addition away.
- **Exception cleanup.** `sink_scope` restores the thread-local sink on unwind, and owning edges are `std::vector`, so removing `nested_rev_autodiff` leaks nothing.

**Questions, ranked by consequence**

1. **Propagator operand order is unverified.** Sink buffer `I` receives propagator edge `I` (`recorder.hpp:407`). This is correct only if the pinned header calls `make_partials_propagator(y, x, alpha, beta, sigma)` in exactly that order. The pinned file exists at `/Users/xitrium/claud/stanrt/deps/math/stan/math/prim/prob/normal_id_glm_lpdf.hpp` but I did not read it. If the order differs, partials land in the wrong scratch slots with no error. Make this the first check.

2. **Row-X geometry versus sink length.** With `row_x`, X is passed as a one-row `Block`, which selects the vector edge with `size() == cols`, while the sink registers `len[1] == rows*cols` (`matrix_fns.cpp:1005`). The emit gate at `recorder.hpp:407` silently skips on mismatch. This is safe only if the lowering guarantees `idata[0] == 1` whenever `idata[4] == 1`. The old code tolerated rows greater than one by writing zeros for the extra rows. The new code would leave stale scratch. Either confirm the lowering invariant or make the recorder's size gate assert instead of skip. I did not read `slot_for`, which decides whether the edge's construction-time `setZero()` also touches scratch.

3. **Scratch is now written before a throw.** Non-owning edges zero their slot on construction (`recorder.hpp:251,306`), and upstream assigns partials before its non-finite check block throws. The old path left scratch untouched on any exception. This is harmless if the executor never runs the backward after a forward throw.

4. **Floating-point parity, where to look if it fails.** The recorder's `value_of` (`recorder.hpp:80-82`) is a lazy unary expression, whereas the var path materialized doubles. Elementwise ops and non-vectorized reductions should match bitwise, and GEMV materializes a lazy right-hand side before the kernel. If the evaluator shows a mismatch in the squared-residual sum or the log-sigma term, the fix belongs in the recorder's `value_of`, not this kernel.

**Scope limits.** I compared only the new `nid_glm_eval` against the old one. I did not read the new file's `nid_glm_bwd` or `nid_glm_scratch` and assume they are unchanged. I did not read `slot_for`, the scalar edge's `size()`, or the pinned upstream header. The two blocked shell commands returned nothing and were not counted against the budget.
