# Cholesky correlation: exact retained scalar reverse

Native baseline db045835. The preceding profile attributes about 21% of
Kronecker GP's instrumented gradient to this constraint, mostly taped reverse.
The graph kernel already mirrors Stan's scalar forward arithmetic; it still
rebuilds a Matrix<var> and replays the transform in backward.

Hypothesis: retain z=tanh(y), each row's prefix squared sum and square root,
and its diagonal value during the existing scalar forward. Reverse those
same scalar operations directly in their original order. This removes var
allocation, reconstruction and dispatch while preserving upstream algorithms.
It adds 3*raw+K doubles per batch plus one eligibility flag, no dependency.

Do not substitute the upstream varmat reverse: it reconstructs prefix sums
from output squares and can round differently from the scalar-var oracle.
Do not simplify the tanh derivative to 1-z*z: the pinned scalar derivative
uses division by cosh(y)^2. Exclude sqrt's zero-guard histories from the fast domain and retain their
taped fallback; preserve primitive accumulation
order, the separately seeded Jacobian and final scatter into an existing
adjoint. Source references are pinned Stan Math prim/constraint/
cholesky_corr_constrain.hpp and corr_constrain.hpp, plus rev/fun/{sqrt,
square,log1m,tanh,dot_product,sum}.hpp.

Alternative: recompute a compact double history in backward. This avoids
retained storage but repeats the existing forward. Compare only if the retained
state adds resolved memory/startup cost; the current tape already allocates
much more temporary state. Another alternative is a varmat substitution, but
that is a different numerical contract and is not this experiment.

Proof domain: valid batch geometry and finite nonsaturated transform history.
Keep the established taped backward for exceptional/saturated history; it is
not MIR interpretation. Forward validation and values-only behavior stay
unchanged. Guard eligibility explicitly and test exceptional refusal so the
fast path cannot silently hide a discrepancy. No model names or data hashes
appear in admission.

Evaluator: exact comparison with the preceding scalar Stan tape across K=0,
1,2,4,8,30, multiple batches, random/mixed/zero seeds, independent weighted
Jacobians, existing adjoints, near-saturation and nonfinite inputs. Assert
eligibility/refusal and value-only/gradient reuse. Check the large fast path
avoids growing the Stan arena. Then full native tests and recorded CmdStan
corpus. Measure six alternating fresh-process pairs on Kronecker GP, an
ordinary small correlated model and unrelated canaries, separately measuring
prep/first/warm/inference/RSS/retained scratch/binary size. Provisional gate:
>=10% native target warm-gradient gain with no resolved ordinary-canary
regression. Keep or revise from measured evidence; do not broaden numerical
tolerances to admit this implementation.

## Correctness checkpoint

The retained scalar reverse passes bitwise comparisons against the preceding
weighted tape, including seeded existing adjoints, ordinary and saturated
histories, 0/1/3 batches and K up to 30. Infinity/NaN seeds and NaN input
rejection are covered. Values-only followed by gradient reuse passes. The full
278-test run found only an assertion in the new zero-batch test: it expected
saturated fallback even though no inputs exist. Corrected the assertion to
require a nonempty batch; the focused rerun passes. All other 277 tests passed
in that full run. The complete 329-model recorded CmdStan corpus passes at
all three points, with all 124 model ULP gates and unchanged worst discrepancy.
No production edit followed those checks. Timing is the next gate.

Fable's review confirmed primitive reverse ordering and requested defensive
scratch-lifetime/geometry cleanup. Those changes are recorded in the adjacent
review note and receive a fresh full test run and final timing artifacts.

The selected ch14_m14_2 uses corr_matrix, not cholesky_factor_corr, so it is an
unaffected correlated-model canary. Add the already prepared s2_mv_shared_re
case for the required small model actually exercising Cholesky correlation.
Measure it in a separate six-pair native/phase batch after the main batch;
do not perturb ongoing timings. Retain both canaries and all initial samples.

The phase benchmark shows +3.55 MB median peak RSS (MAD1.79 MB) for unrelated
ch14_m14_2, which uses corr_matrix and cannot allocate the new retained state.
That process also repeatedly runs the embedded OCaml source compiler. Test the
hypothesis that the signal belongs to source/compiler/allocation history rather
than native model execution with six paired /usr/bin/time -l bench_grad runs on
kronecker_gp, ch14_m14_2 and s2_mv_shared_re, using already prepared MIR. Retain
both process boundaries and report disagreement; do not erase the original RSS
signal. Run after the current timing batch to avoid contention.
