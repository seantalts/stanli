# Normal identity GLM: direct partial recording

Baseline c3d0f929. The kernel now invokes the same upstream Stan Math
normal_id_glm_lpdf implementation with the existing recording scalar. It writes
analytical partials into the existing scratch buffers without building or
walking a temporary var tape. Activity, scalar/vector and row/matrix overloads,
propto terms, scratch layout and backward contraction remain the same. The
previous initial positive-zero adjoint accumulation is preserved explicitly.
Registry diagnostics now identify this kernel as recorded_partials.

The [profile/evaluator](2026-09-28-native-kernel-profile.md) explains selection
and proof obligations. [Fable's review](2026-09-28-fable-normal-glm-review.md)
found no confirmed defect; its questions about upstream operand order,
row-vector dimensions and exception readiness were checked against source and
the exact differential evaluator.

## Correctness

The new differential test compares values, stored partials, weighted scatter,
exceptions and reuse against the preceding var implementation. It covers
empty and nonempty shapes, zero columns, eight activity masks, scalar/vector
outcomes/intercepts/scales, row-vector broadcasting, full/propto and nonfinite
weights/inputs. Comparisons are bitwise, allowing equivalent NaN classification.
The large active-design case also verifies that the kernel no longer grows the
Stan arena. Source inspection confirms no nested tape remains in this path.

All 278 configured native tests pass. The complete recorded CmdStan corpus
passes: 329 models, three points, 1,020,194 values, all 124 model ULP gates.
Worst scaled error remains 9.38e-13 / 7040 ULP; the corpus's documented gates
are not a universal ten-ULP guarantee. Native public-API benchmark gradients
also compare exactly between libraries. The interface ABI did not change.

## Native results

Six counterbalanced fresh-process pairs, Release Apple Clang 21 arm64, 200 ms
warmup and 250 ms measurement. Direct native bench_grad excludes Python call
overhead. Medians below are nanoseconds per gradient (MAD in parentheses).

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| sw_gaussian | 363.5 (8.2) | 263.3 (6.9) |
| diamonds | 34452.9 (569.9) | 34288.7 (197.7) |
| eight_schools_centered | 253.7 (6.8) | 253.4 (4.6) |
| eight_schools_noncentered | 237.9 (8.0) | 225.2 (2.6) |

Small-model warm gradients improve 27.6%. Diamonds is effectively unchanged:
the kernel dominated its profile but its matrix arithmetic, not removable tape
bookkeeping, dominates the kernel. Do not extrapolate the small-model gain.
[Native samples and executable hashes](2026-09-28-normal-glm-native-performance.json).

The independent public-C-API phase benchmark includes ctypes overhead. Small
Gaussian warm gradients change from 0.974 to 0.873 microseconds; preparation
169.0 to 165.2 microseconds; first gradient 31.2 to 26.5 microseconds. Short
inference (100 warmup, 100 draws, plus rows) changes from 1.263 to 1.101 ms,
a 12.8% reduction. Diamonds inference is 1.724 versus 1.717 seconds. Source
compilation and preparation show no resolved slowdown. These are small seeded
inference experiments, not posterior-efficiency or convergence claims.

The initial unrelated canaries show noisy short-inference increases, +5.1%
centered and +9.5% noncentered. A predeclared bounded follow-up used eight
identical-binary A/A and eight A/B pairs per canary with warmed repeated
sampling. Centered inference ratios are A/A 0.986 (MAD 0.026), A/B 0.982
(0.025); noncentered A/A 1.032 (0.031), A/B 1.045 (0.046). All draw hashes
agree. The overlapping controls do not resolve a slowdown, and they do not
prove its absence. In particular, warmed sampling is narrower than the initial
first-inference-plus-row boundary. Retain the original signal as an evidence
limit rather than hiding it or running open-ended confirmations.
[Phase samples](2026-09-28-normal-glm-performance.json),
[canary controls](2026-09-28-normal-glm-canary-confirmation.json).

The library shrinks by 475,296 bytes (40,101,760 to 39,626,464), and gzip by
33,900 bytes (12,385,847 to 12,351,947). Scratch/retained storage is unchanged;
the temporary var allocations disappear. Whole-process peak RSS differs by
about 25–107 KB across these fixtures, too small to claim a measured RSS gain.
No dependency or installation requirement was added.

The candidate clears the provisional native target improvement gate without
a resolved canary regression. Integrate this bounded kernel change and continue
to the separately profiled Cholesky-correlation transform. Its taped reverse is
about 21% of the Kronecker GP profile. That next slice must preserve the scalar
Stan reverse arithmetic; substituting the varmat formula can change rounding
and saturation behavior. [Post-change profiles](2026-09-28-normal-glm-after-profile.json)
retain the new attribution rather than assuming the original cost shares.
