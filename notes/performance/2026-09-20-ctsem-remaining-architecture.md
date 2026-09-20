# Remaining ctsem architecture results

The selected changes reuse active plain-left QR factors, reuse temporary
recording buffers, combine version metadata, and fuse eligible data-call/branch
dispatch. On the 4,000-row target, warm gradients fall from 474.4 to 411.8 ms
and the first gradient from 15.224 to 14.018 s. Median paired reductions are
13.5% and 7.8%, respectively. Retained live allocation increases by 32.8 MB.

These comparisons start from the already validated direct-forward checkpoint
`22c2d86b`, on fetched upstream `680a4763b256764476bca72b7916ad312c910701`.
They are additional gains over the [previous solve-forward work](2026-09-20-ctsem-solve-kernels.md).
They measure evaluations, not complete sampling or source compilation.
Implementation commits are `cacd49e3` (solve isolation and QR retention) and
`ba390955` (recording changes). The final built library is byte-identical to
the measured selected archive.

## Implementation and attribution

The solve family now has its own translation unit, with the original compile
options. This separates its template code generation from unrelated matrix
and density kernels. Active plain-left forwards save the original packed QR
and Householder coefficients in per-execution scratch, and backward reconstructs
the original Eigen matrix/vector expression types. The numerical operations,
validation and adjoint accumulation order are unchanged. Null-scratch and
inactive callers retain recomputation. Right and SPD solves do not retain
factors. In particular, a near-symmetric SPD input is not permission to replace
the backward triangle with the forward triangle.

Frame recording reuses cleared staging vectors and sealing scratch; completed
frames still own their values and code. Scratch is released on completion.
After a recording exception, it is discarded on the next forward's failed-tape
reset or executor destruction. Combining each version's value, adjoint, owner
and constancy removes two separate pushes. The combined record is 24 bytes
versus 21 bytes across the previous pools, and admission estimates use that
actual size. The observed target modes remain streams at N32/N33 and frames
at N400/N4000.

The dispatcher fuses a data-only call with an immediately following Branch
or WhileTest only after the existing immutable-output and activity proofs.
It executes the same kernel and publishes the same value. The original branch
instruction remains available to independent jumps. There are 444 eligible
pairs in the target; no model names or source-variable names select any change.

Six balanced three-way rounds at N4000 compare direct-only, isolated QR,
and the complete selection. Recording changes alone reduce first-gradient
time from 15.207 to 14.018 s: median ratio 0.9242, bootstrap 95% interval
0.9163–0.9354. Their warm ratio is 0.9968, interval 0.9844–1.0069, and live
allocation is identical. QR retention accounts for the warm improvement;
the recording changes account for the startup improvement.

Earlier two-pair N400 feasibility ablations isolate buffer reuse
(first gradient 1.5648→1.5401 s), combined versions (1.5496→1.5192 s), and
branch dispatch (1.5118→1.4590 s). Those probes select experiments; the final
six-pair measurements support the combined claim.

## Target measurements

Apple M3 Ultra, Apple clang 21, arm64 Release, `-ffp-contract=off`, pinned
dependencies, one thread, identical benchmark-driver object. Six fresh-process
pairs per size, 300 ms warmup and 1,000 ms measurement; no observations removed.
At N4000 all six permutations of the three variants balance order. Intervals
use 10,000 bootstrap paired medians with seed 20260920. Builds and correctness
jobs run separately from performance measurements.

| Rows | Baseline warm ms | Selected warm ms | Paired ratio | Bootstrap 95% interval |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 3.392 | 2.876 | 0.8455 | 0.8384–0.8536 |
| 33 | 3.447 | 2.925 | 0.8504 | 0.8434–0.9382 |
| 400 | 46.329 | 40.164 | 0.8690 | 0.8587–0.8712 |
| 4,000 | 474.438 | 411.757 | 0.8650 | 0.8624–0.8854 |

| Rows | Baseline first gradient s | Selected first gradient s | Paired ratio | Bootstrap 95% interval |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 0.2362 | 0.2321 | 0.9754 | 0.9466–1.0241 |
| 33 | 0.2594 | 0.2592 | 1.0030 | 0.9717–1.0688 |
| 400 | 1.5600 | 1.4494 | 0.9261 | 0.9201–0.9383 |
| 4,000 | 15.2238 | 14.0181 | 0.9218 | 0.9101–0.9307 |

The small stream cases do not establish a first-gradient improvement. N4000
preparation from saved MIR is 1.386 / 1.373 s. First-gradient time excludes
that preparation and includes recording plus the first reverse pass.

At N4000, warmed live allocation is 1,408,061,840 / 1,440,895,440 bytes, up
32,833,600. Median process peak RSS is 1,518,256,128 / 1,542,971,392 bytes,
up 24,715,264. These are different measures; neither is a count of allocation
calls. Saved frame cells rise from 76,716,918 to 83,100,218. Frame count
(3,995), unique programs (4), code words (638,725), and bindings (2,655,409)
remain unchanged.

One separate 20-evaluation diagnostic splits the warm work: forward is
220.1 / 219.1 ms, reverse is 255.0 / 190.6 ms. These single-process means
are separate from the paired gate and support the expected reverse-only
benefit of saved factors.

## Ordinary-model controls and uncertainty

The solve split leaves the unrelated matrix/density object byte-identical
across split-only, retained QR and recording variants:
`4d1a1bca9ed9e5bfa322fba73d12506043a0ea7cbe650779c3d41ad2b65490a7`.
The actual `lotka_volterra` and `s2_mv_subset` graphs contain zero solve calls.
This excludes new solve work and recompilation of those bodies, but does not
establish the precise cause of every cross-binary timing fluctuation.

All seven unrelated controls in the final eight-model regression set have
six-pair intervals containing parity. The solve fixture improves 6.8%, interval
0.9180–0.9490. In the separate seven-model canary set, the solve fixture improves
6.0%; five unrelated intervals contain parity. The remaining GP canary initially
shows ratio 1.0066, interval 1.0025–1.0133.

Before the exploratory screen completed, a fixed follow-up policy selected
that GP case and at most the three largest >5% screen signals not already
covered by six-pair controls. Each received six identical-binary A/A pairs and
six A/B pairs, with no further adaptive reruns:

| Model | Follow-up A/B ratio | A/B 95% interval | A/A-adjusted 95% interval |
| --- | ---: | ---: | ---: |
| dogs_hierarchical | 1.0100 | 0.8771–1.1610 | 0.8655–1.1777 |
| kronecker_gp | 1.0076 | 0.9961–1.0145 | 0.9918–1.0239 |
| s2_ar_cov | 0.9891 | 0.9792–1.0020 | 0.9831–1.0101 |
| s2_mi_lognormal | 1.0102 | 0.9827–1.0237 | 0.9789–1.0268 |

These follow-ups do not establish a repeatable regression; they also do not
prove zero cost. The dogs fixture is especially noisy in both A/A and A/B.
The earlier isolated-QR `s2_mv_subset` result remains inconclusive: ratio
1.0238, raw interval 0.9935–1.0646, adjusted interval 0.999985–1.0715.
The final selected result is 0.9962, interval 0.9930–1.0150. Both observations
are preserved, without attributing a cause or discarding an inconvenient run.

The 322-case one-pair screen has 319 finite bitwise matches and the same
three nonfinite points (`dogs_log`, `s2_invgaussian`, `sir`). Its median ratio
is 1.00515 and geometric mean 1.00308, with range 0.8732–1.2415; 84 cases
exceed 1.05 and 65 fall below 0.95. These short 100 ms warmup / 250 ms windows
are exploratory and do not establish universal per-model performance parity.
The unrelated six-pair controls have 64 additional live bytes; the solve
fixture has 384 additional bytes.

## Other plan items

The current first-recording profile motivated buffer and dispatch work:
7,661 samples include 3,421 exclusive samples in recording dispatch and
582 in frame sealing. A separate line-aware profile of the combined-version
candidate resolves instruction-loop dispatch, data operand rebinding and
comparison work. Profiling runs are not included in the performance gate.

Recorded clone reuse is deferred after inspecting the C API sampler,
initialization and executor pool. Normal clones precede recording. A cold-source
clone takes 0.193 ms, but its first gradient takes 15.205 s. Cloning a recorded
source takes 0.412 ms and the clone records again in 15.085 s. Two recorded
executors reach about 3.01 GB RSS. Reuse would need a new recording boundary
and relocation of mutable storage; only about 5 MB of immutable code can be
shared directly, versus roughly 614 MB of mutable cells and 2.66M bindings.
This is a measured startup opportunity, not an implemented sampling gain.

Automatic subject parallelism is deferred after a diagnostic reference-closure
probe. The actual MIR has no reduce_sum, and current lowering refuses both
retained/reduce_sum nesting orders. Of 3,995 frames, 433,132 value references
and 433,132 adjoint references cross a non-prefix frame boundary, with maximum
distance one and 3,992 coupled boundaries. The conservative partition yields
only two runs. Shared prefix and workspace writes still need a proof. The
[diagnostic patch](2026-09-20-ctsem-frame-dependency-probe.patch) is retained,
without enabling parallel replay.

The matrix-exponential prototype implements the dedicated Fréchet algorithm
from [Al-Mohy and Higham](https://eprints.maths.manchester.ac.uk/1218/).
The target has 2,001 extent-10 exponential calls per gradient. Four fixed
extent-10 microcases show 36–46% kernel improvement, but the existing accuracy
gate fails on non-normal matrices: in the 5x5 Jordan-like case with off-diagonal
100, 60-digit oracle scale error worsens from 2.38e-15 to 1.60e-13. All 47
oracle cases and the [prototype patch](2026-09-20-ctsem-frechet-probe.patch)
are preserved. Production retains the block-exponential method; no tolerance
was relaxed.

## Numerical and lifetime validation

All four target sizes at three points match the established implementation
bitwise (581 values per point). Worst target scaled CmdStan difference is
3.2102e-13 against the existing 1e-9 gate. Changing points 0→1→2→0 with
interleaved value-only calls also remains bitwise.

The 329-model reference gate passes three points each and 1,020,194 values,
with worst scaled error 9.38e-13. Full CTest passes 262/264. The failures are
`test_density_signature_model_generation` and
`test_builtin_signature_model_generation`, both reproduced on the untouched
baseline. No manifests, exceptions or numerical budgets changed.

Exact solve coverage includes all operand activities, vector/matrix shapes,
null scratch, zero sizes, nonfinite and ill-conditioned inputs, and validation
order. Lifetime fixtures change the divisor at a repeated site, cover forced
streams/frames, guard changes, copied executors, interleaved value-only calls,
top-level calls, generated island adjoints and private var-replay call arenas.
The forward oracle retains 3,024 exact comparisons. Validation is local arm64;
x86 execution remains for CI.

The additional dispatch fixtures pass after review. They enter a retained
Branch directly before its producer's first publication and on later frame
trips, and enter a retained WhileTest via `continue` inside the condition
block. Changed data forces recording again; values and gradients compare
bitwise with the unspecialized recorder and tree evaluator.

Fable 5.1 reviewed the [architecture](../../docs/superpowers/plans/2026-09-20-ctsem-remaining-fable-review.md),
[implementation](../../docs/superpowers/plans/2026-09-20-ctsem-remaining-implementation-review.md)
and [dispatch delta](../../docs/superpowers/plans/2026-09-20-ctsem-remaining-dispatch-review.md)
through the read-only Claude CLI. It found no actionable correctness defects.
CLI initialization and every assistant response identify `claude-fable-5-1`;
the CLI also reports ancillary Haiku usage. There were no tool denials.

The [updated plan](../../docs/superpowers/plans/2026-09-20-ctsem-remaining-architecture.md)
records each disposition. The [machine-readable scorecard](2026-09-20-ctsem-remaining-architecture.json) preserves identities,
raw paired observations, numerical summaries, ablations, ordinary controls and
probe results. Raw scripts, libraries, inputs and logs remain under
`.cache/ctsem-remaining/` and `.cache/ctsem-post-392/`.
