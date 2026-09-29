# ctsem solve-kernel results after PR392

Follow-up: [remaining architecture results](2026-09-20-ctsem-remaining-architecture.md)
record the later isolated QR and recording implementation. This report
preserves the earlier direct-forward checkpoint and its rejected prototypes.

The final patch removes unused reverse-tape construction from eligible active
solve forwards while preserving their numerical algorithms. At 4,000 ctsem
rows, warmed gradients fall from 501.4 to 473.6 ms. The median paired reduction
is 5.4%, with a bootstrap 95% interval of 4.4–6.0%. It adds no retained storage.
Two QR-retention prototypes achieved larger target gains but were excluded
because ordinary-model slowdowns remained unresolved.

The fetched base is PR392's merge,
`1449c018406e961bc5a3b103740bd122b113fd29`. Fable 5.1 reviewed the revised
[plan](../../docs/superpowers/plans/2026-09-20-ctsem-post-392.md) and
[implementation](../../docs/superpowers/reviews/2026-09-20-ctsem-solve-implementation-fable.md)
through the `claude` CLI. It found no blocking correctness issues. Selection
between the numerically correct stages followed the performance experiments.

## Selected implementation and matched measurements

Active plain-left, SPD-left/right and triangular-left forwards now execute
the pinned double-valued numerical work directly. They preserve activity-
specific expressions, Eigen vector/matrix shapes, validation order and
exception behavior. Active plain-left still uses QR; its value-only path
still uses LU. Right plain/triangular solves retain the established path.
Eligibility uses family, shape and operand activity, with no model recognition.

Both libraries use pinned dependencies, Apple clang 21 Release build,
`-ffp-contract=off`, and the same benchmark-driver object on an Apple M3 Ultra.
Each size has six alternating fresh-process pairs, 300 ms warmup and 1,000 ms
measurement. No observations were excluded. All returned values match bitwise.
The final build's actual runtime object members match the measured direct-only
archive byte for byte; archive metadata and symbol-index bytes are excluded
from that comparison.

The [machine-readable scorecard](2026-09-20-ctsem-solve-kernels.json) preserves
source/input/binary identities, individual target observations, ablations,
controls, numerical results and commands. Raw logs and evaluator scripts
remain in `.cache/ctsem-post-392/`; external model/data files stay there.

| Rows | Base warm ms | Selected warm ms | Median paired ratio | Bootstrap 95% ratio interval |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 3.629 | 3.400 | 0.9378 | 0.9288–0.9480 |
| 33 | 3.650 | 3.450 | 0.9477 | 0.9348–0.9502 |
| 400 | 49.076 | 46.315 | 0.9403 | 0.9371–0.9592 |
| 4,000 | 501.398 | 473.565 | 0.9456 | 0.9397–0.9558 |

At 4,000 rows, preparation from existing MIR is 1.389 / 1.389 s, and first
gradient is 15.220 / 15.194 s; their paired intervals include parity. Source
compilation was not remeasured. This establishes a warm-evaluation gain, not
a first-recording or complete-inference speedup.

Warmed live allocation is 1,408,224,672 / 1,408,061,840 bytes, down 162,832.
Median peak RSS is 1.525 / 1.522 GB, with a paired ratio interval spanning
parity. No scratch hook, factor cache or other retained state is added.

A separate diagnostic times forward and reverse over 20 warmed evaluations
per binary. Forward falls from 246.8 to 220.9 ms; reverse is 251.2 / 253.1 ms.
These single-process averages are separate from the paired performance gate
and agree with the intended forward-only change. The retained-frame census
is unchanged: 3,995 frames, four programs, 638,725 code words, 76,716,918 cells
and 2,655,409 bindings.

## Why QR retention is deferred

The first retention prototype saves each plain-left forward's packed QR and
Householder coefficients in existing per-execution scratch, then uses
map-typed expressions in backward. It is bitwise correct but introduces
repeatable small slowdowns on four unrelated controls. Reconstructing the
factor into the original MatD/VecD expression types resolves those four
signals and preserves the target gain: full-size medians are 497.6 / 413.3 ms,
with a median paired reduction of 17.4%, at a cost of 32.7 MB live allocation.

The second prototype still fails the broader performance gate. A fixed final
six-pair A/B and six-pair identical-binary A/A confirmation of the four largest
screening spikes yields these residual signals:

| Model | A/B median ratio | A/B 95% interval | A/A-adjusted 95% interval |
| --- | ---: | ---: | ---: |
| lotka_volterra | 1.0725 | 1.0420–1.0936 | 1.0008–1.0824 |
| s2_mv_subset | 1.0219 | 1.0011–1.0525 | 1.0076–1.0670 |

The direct-only ablation is neutral on both (six-pair ratios 0.9924 and
0.9927, with intervals including parity), as it is on the earlier four
controls. The retention stage is therefore excluded. Link layout or shared
template code generation is a hypothesis, not established attribution; the
near-neutral inventory aggregate does not excuse these per-model signals.

The [archived retention patch](2026-09-20-ctsem-qr-retention.patch) applies
cleanly over the selected implementation and includes its lifetime tests.
It is experimental data, not enabled runtime code. Full libraries, binaries,
source snapshots and raw measurements remain under `reuse/` and
`reuse-matrix/` in the artifact directory. Its exact cached/recomputed
pullbacks, changing-divisor tests, forced streams/frames, guard re-recording,
value-only alternation and executor-copy tests all passed. Performance,
not numerical correctness, prevented integration.

## Selected ordinary controls and correctness

Seven six-pair canaries cover scalar, HMM, GP, solve and retained-loop
workloads. The solve fixture improves 6.4% (paired ratio interval
0.9253–0.9499), with 16 fewer warmed live bytes; the six unrelated canaries
have equal live allocation in both builds.
Five unrelated canary intervals include parity. The mixed-cell canary's
initial ratio is 1.0145 (interval 1.0031–1.0545). Its one fixed six-pair A/B
and identical-binary A/A follow-up gives ratio 1.0080, interval
0.9876–1.0235, and A/A-adjusted interval 0.9776–1.0350. That residual signal
is inconclusive; no further adaptive timing rounds are used.

The 322-case exploratory screen has 319 finite bitwise matches and the same
three nonfinite points (`dogs_log`, `s2_invgaussian`, `sir`) in both binaries.
Median ratio is 1.00779 and geometric mean is 1.00122. Single-pair ratios range
from 0.7999 to 1.1186; those short windows do not establish universal
per-model performance parity. All six cases implicated by the rejected QR
variants have neutral six-pair direct-only intervals.

The forward oracle passes 3,024 exact overload comparisons, including
activity combinations, vector/matrix shapes, empty and ill-conditioned
inputs, nonfinite values, signed zero and exact exception behavior. Existing
gradient budgets remain unchanged; the sweep now includes legacy activity
zero. The selected implementation changes no backward formula or lifetime.

All four target sizes at three parameter points match the base bitwise
(581 values each). The largest scaled CmdStan error is 3.2102e-13 at 4,000
rows, point 0, below the existing 1e-9 gate. A changing-point sequence
0→1→2→0 with interleaved value-only calls also matches bitwise.

Reference replay on the final selected build passes all 329 models at three
points and 1,020,194 values, with worst scaled error 9.38e-13. Full CTest
passes 261/263. The two signature-manifest freshness failures reproduce on
the untouched base; generated manifests are unchanged. Validation is on
Apple arm64; x86 bitwise execution remains for CI.

The selected warm profile has 7,723 main-thread samples. Solve-forward stacks
occupy 15.1%, versus 18.9% in the base; backward remains 18.2%. Matrix
exponentials together occupy 4.4%, so changing their derivative algorithm
remains a lower-priority, untested alternative. The rejected original-type
QR prototype reduces solve-backward's sampled share to 5.6%, preserving
evidence that the larger numerical opportunity remains worth investigating.

The next QR experiment must explain or eliminate the ordinary-model cost
before enabling retention. Separating shared-template code generation from
solve-local work is a concrete hypothesis; arbitrary binary padding and
repeated timing rounds would not answer it. The matrix-exponential derivative,
subject parallelism and recorder changes remain separate, unimplemented
alternatives. The revised plan's measured direct-forward stage is complete.
