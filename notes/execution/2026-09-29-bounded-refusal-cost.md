# Avoiding failed nested-array compilation trials

## Decision

Keep the nested real-array extension in #416 with an earlier resource proof.
The previously measured length-128 refusals no longer construct an isolated
trial. Their preparation and complete first-use results do not resolve a
regression against merged main. Supported targets retain their large output
speedups. This is a bounded measurement result, not a guarantee for every model.

The original [nested-array results](2026-09-29-bounded-nested-arrays.md) remain
historical evidence, including their setup regression and inconclusive cold
inference comparison. This follow-up supersedes that review limitation for the
two measured resource refusals; it does not remove all speculative lowering.

## Cause, proof and alternatives

A four-second native preparation profile placed the extra work inside the
failed trial's lowering, especially indexed-update construction and exception
unwinding. Trial-state copying was not the dominant sampled cost.

The first candidate checked the existing slot limit before lowering indexed
assignment operands and skipped real-range analysis in bounded output blocks.
Real ranges prove while-loop termination; this closed grammar admits only
counted loops. Integer range and initialization proofs remain enabled. These
changes reduced the measured extra preparation but left about 50 microseconds,
so they were insufficient alone.

The final check follows straight-line integer definitions and snapshots local
nested-array dimensions at declaration. It uses the same interval maxima and
loop multiplicities as bounded lowering. An indexed update necessarily needs
a result slot as wide as its base container: if that slot alone exceeds the
unchanged loop-weighted limit, the trial cannot succeed. Refuse before forking.

The check is deliberately incomplete. It does not admit programs, grow budgets,
execute a new runtime, or change array layout. Unknown bounds and control defer
to existing lowering. Repeated scalar writes invalidate inferred bounds.
Only data-decided branches are traversed, including provable logical short
circuits; mutable inputs cannot establish their conditions. Empty loops are
skipped. Empty dimensions, narrow containers, and large arrays without repeated
writes can still compile. Failed estimates preserve the normal trial path.
The existing transactional fallback preserves the complete parent graph.

A complete second range/shape analysis would duplicate more compiler machinery;
it was not needed to remove the measured penalty. Increasing budgets would
change admission and memory policy, so it was not used. The small early-slot
check also helps refusals the preflight cannot prove. No experimental flags,
inactive kernels, or alternative execution engines are retained.

## Correctness

- All 324 CTests pass, including the independent CmdStan nested-array references.
- All 329 recorded models pass: 1,020,194 values, existing gates unchanged.
  The previously documented 7040-ULP cancellation exception remains; this is
  not a universal 10-ULP claim.
- All 373 prior execution selections match the preceding #416 head.
- Another 405 two-/three-/five-dimensional cases vary outer capacity, inner
  width, repeated fills and data/iterator-dependent branches. Output execution
  reports are unchanged in every case; 30 now refuse before trial construction.
- Regression tests compare complete output rows and RNG continuation for empty
  loops, empty inner shapes and narrow arrays at large outer capacity. Refusals
  compare the complete compiled prefix with structured admission disabled.
  Scoped construction counts verify that the measured oversized cases avoid
  constructing a trial at all. Formatting passes.

The first corpus invocation used a nonexistent checkout-local posterior corpus
and reported 119 missing inputs. The corrected invocation uses the established
`/Users/xitrium/claud/stanrt/deps/posteriordb` source checkout; both logs are retained.

## Native measurements

Darwin arm64, AppleClang 21, Release, threads enabled, full LP. Main baseline:
`bd118b2d85ba56df4e0372e07bceef6379168651`, library SHA-256
`8fe5ecd7ab18680e07312ece96548de29be3eec7620930588f228300ac5bb794`.
The preceding #416 head is `e8365c5bc3e9c513c58ccb61edfe1713589d191f`.
The archive records the exact candidate source diff and build configuration.

Each final comparison has six alternating fresh-process pairs. Phase windows
use 200 ms warmup / 250 ms measurement. Cold inference includes preparation
from MIR, 100 warmup iterations, 100 posterior draws, all output rows and
model destruction; it excludes library loading, file reads and source compilation.
Values below are median ± MAD.

| Preparation, microseconds | Main | Final candidate |
| --- | ---: | ---: |
| Two-dimensional length-128 refusal | 298.477 ± 5.636 | 291.663 ± 1.671 |
| Three-dimensional length-128 refusal | 322.671 ± 9.104 | 330.601 ± 4.799 |
| Two-dimensional compiled target | 304.667 ± 6.531 | 337.952 ± 8.324 |
| Three-dimensional compiled target | 372.328 ± 10.837 | 382.131 ± 13.828 |

Supported targets retain setup costs, recovered in ordinary inference:

| Cold complete inference, milliseconds | Main | Final candidate |
| --- | ---: | ---: |
| Two-dimensional target | 7.836 ± 0.348 | 1.233 ± 0.055 |
| Three-dimensional target | 14.080 ± 0.094 | 1.431 ± 0.019 |
| Two-dimensional length-128 refusal | 86.723 ± 2.853 | 83.598 ± 1.196 |
| Three-dimensional length-128 refusal | 180.673 ± 2.670 | 181.685 ± 8.979 |
| Ordinary AR1 | 8.447 ± 0.452 | 8.110 ± 0.151 |
| Prior integer block | 1.038 ± 0.038 | 1.039 ± 0.025 |
| Changing-inner-dimension refusal | 1.551 ± 0.007 | 1.545 ± 0.012 |
| Nested ODE callback | 69.348 ± 2.717 | 68.772 ± 3.398 |

Compiled target warm rows improve 30.052 → 1.253 microseconds and
51.969 → 1.996 microseconds (24.0× and 26.0×). Cold inference improves
6.4× and 9.8×. No measured canary resolves an end-to-end slowdown.

A separate changing-input comparison alternates empty and maximum-length rows.
Cold preparation plus the first empty row is 700.750 ± 34.667 →
683.625 ± 23.354 microseconds for two dimensions, and 716.167 ± 33.166 →
711.813 ± 28.250 for three. The prior roughly 0.15 ms cold-setup penalty is
absent in these measurements. Subsequent changing-row timings are
655.830 ± 25.831 → 641.370 ± 20.796 and
1342.593 ± 20.425 → 1367.351 ± 27.427 microseconds; both still execute MIR.
Do not interpret those noisy differences as compiled-path gains.

The final library is 39,856,384 bytes: +4,880 versus main and +3,536 versus
the preceding #416 library. SHA-256:
`5fa2c40b3916db76a239d06898cc37243c20f5cc10946e146876a154d6265522`.
Whole-process peak RSS samples are retained; this lowering-only change does
not measure retained executor memory or compressed package size.

One final phase group accidentally overlapped the first cold run. The entire
cold run and that phase group were excluded and repeated sequentially, before
using their results for a decision. Originals remain in the archive. No
sign-seeking repetitions or tolerance changes were used.

[Raw evidence](data/2026-09-29-bounded-refusal-cost.json.gz) includes both
intermediate candidates, final measurements, excluded overlapping runs,
profiles, source/binary/input identities, test logs, and runner scripts.
The maintained execution benchmark manifest now includes both budget-refused
shapes so future admission work measures their setup cost as well.
