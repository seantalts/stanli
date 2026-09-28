# Retained Cholesky-correlation reverse

Baseline db045835. For finite nonsaturated histories, the constraint now keeps
the tanh values, row-prefix sums, square roots and diagonals from its existing
scalar forward, then reverses the pinned Stan scalar primitive sequence
directly. It preserves Jacobian weighting, accumulation order and the scalar
cosh-based tanh derivative. Saturated/nonfinite histories retain the previous
Stan tape. This removes a local differentiation tape, not MIR interpretation.

[Proof packet and evaluator](2026-09-28-cholesky-correlation-plan.md),
[Fable review and dispositions](2026-09-28-fable-cholesky-correlation-review.md).
Fable confirmed the scalar arithmetic and prompted defensive cleanup of
values-only invalidation, scratch geometry and batch strides. Per-executor
scratch remains private; no shared mutable retained state was introduced.

## Correctness

Bitwise internal comparisons cover K=0/1/2/4/8/30, zero/one/three batches,
ordinary, near-saturated and exceptional inputs, independent Jacobian and
output weights, existing adjoints, tiny/large/nonfinite seeds, and reuse after
value-only execution. Equivalent NaN classifications are accepted. Tests
assert fast-path admission and exceptional fallback, and observe no Stan arena
growth for the large retained-reverse case. No tolerance was widened.

The final reviewed build passes all 278 native tests. The complete recorded
CmdStan corpus passes after the review edits: 329 models, three points,
1,020,194 values, all 124 model ULP gates; worst scaled discrepancy remains
9.38e-13 / 7040 ULP. These documented corpus gates are not a universal ten-ULP
bound. Public-API benchmark log densities and gradients match the baseline
exactly at their evaluation point.

## Performance

Six counterbalanced fresh-process pairs per case, Release Apple Clang 21 arm64,
200 ms warmup / 250 ms native measurement. Direct bench_grad medians in
microseconds (MAD in parentheses):

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| Kronecker GP, K=30 | 201.855 (4.079) | 161.606 (5.802) |
| Small shared correlated effects, K=2 | 3.370 (0.079) | 3.103 (0.077) |
| Correlation-matrix canary | 31.157 (0.703) | 30.594 (1.195) |
| Small Gaussian canary | 0.271 (0.005) | 0.258 (0.005) |
| Centered eight-schools canary | 0.252 (0.005) | 0.256 (0.003) |

The final native target improves 19.9%; the first prototype measurement was
14.6%, before the defensive review edits and at a different measurement time.
The small affected model improves 7.9%. Unaffected native canary shifts are
small compared with their dispersion; do not attribute their improvements to
this kernel. [Native samples](2026-09-28-cholesky-correlation-native-performance.json),
[small-model native samples](2026-09-28-cholesky-correlation-small-native-performance.json).

The separately measured public-C-API target gradients improve 16.5%, from
192.756 to 160.997 microseconds. Target preparation changes from 8.846 to
8.985 ms (+0.139 ms, +1.6%); first gradient from 317.6 to 292.5 microseconds.
The small affected model's preparation changes from 1.222 to 1.252 ms, with
overlapping dispersion; first gradient 64.75 to 65.96 microseconds. Source
compilation is unchanged in code and shows no resolved shift. Do not claim
uniformly zero startup cost.

Complete short inference includes 100 warmup, 100 sampled draws and all output
rows. Kronecker GP improves from 38.938 s (MAD0.098 s) to 32.802 s (0.033 s),
15.8%. The small affected model improves from 31.473 ms (1.416 ms) to
28.327 ms (0.223 ms), 10.0%. The correlation-matrix canary is 501.5 versus
504.9 ms; Gaussian and eight-schools inference medians decrease. These are
seeded timing runs, not convergence or posterior-efficiency claims.
[Phase samples](2026-09-28-cholesky-correlation-performance.json),
[small-model phase samples](2026-09-28-cholesky-correlation-small-performance.json).

## Memory, size and limits

Retained state adds `1 + batches*(3*raw + K)` doubles per executor/op: 10,688
bytes for one K=30 transform and 48 bytes for K=2. This replaces temporary tape
work, but it is additional retained storage and scales with batch count. Large
arrays of transforms were not performance-benchmarked in this slice.

Whole-process RSS in the phase benchmark includes repeated embedded source
compilation. It showed an unrelated canary increase of 3.55 MB with 1.79 MB MAD.
A declared follow-up using precompiled MIR and native executables did not
reproduce that magnitude: target peak RSS changes 11,018,240 to 10,977,280
bytes; small affected model 8,167,424 to 8,192,000; unrelated correlation-matrix
canary 11,886,592 to 12,042,240. The latter still rises 155,648 bytes despite
having no new retained state. Its cause is not isolated; do not claim a general
RSS reduction or erase the original process-level signal.
[Native memory samples](2026-09-28-cholesky-correlation-native-memory.json).

Library size increases 96 bytes, 39,626,464 to 39,626,560. gzip increases 1,161
bytes, 12,351,947 to 12,353,108. No dependency or installation requirement was
added. The measured throughput benefit is worth the explicit retained-storage
tradeoff in the tested cases; broader platform and large-batch performance are
unclaimed.

[Post-change profile](2026-09-28-cholesky-correlation-after-profile.json) shows
this constraint dropping from about 21% to 5.5% of instrumented Kronecker GP
time. Symmetric eigendecompositions now dominate. Continue the separately
reviewed callback matrix-geometry coverage experiment; do not reopen native
instruction generation or JIT research.
