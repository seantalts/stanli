# Educational corpus results — 2026-09-14

All 13 models passed the three-point CmdStan oracle (6,609 scalar comparisons), complete sampling CSV checks, and the sampled-parameter mean comparison. Worst scaled pointwise error: 8.30e-15. **Ten pass the 0.5x speed floor; three fail.** No performance failures are suppressed.

Apple M3 Ultra, macOS arm64, Apple Clang 21, Release. Stanli base `2448aa1d` plus the optional phase-timing CLI change and this harness. CmdStan uses `--O1`, `-O3`, and `-ffp-contract=off`. See [raw results](benchmark-results.json) for toolchain identities, paired raw timings, dispersion, seeds and all parameter mean checks.

Median of three alternating pairs after one warmup pair; each chain uses 1000 warmup + 1000 saved draws. Both engines write full 17-digit CSVs. The speed ratio includes Stanli source compilation and preparation versus already-compiled CmdStan; CmdStan build time is recorded separately.

| Model | Stanli CLI (ms) | CmdStan CLI (ms) | Speed vs CmdStan | Floor |
|---|---:|---:|---:|---|
| aalto_bern | 21.02 | 16.67 | 0.793x | PASS |
| aalto_binom | 17.29 | 12.77 | 0.739x | PASS |
| aalto_binom2 | 20.21 | 14.98 | 0.741x | PASS |
| aalto_binomb | 18.86 | 13.41 | 0.711x | PASS |
| aalto_gpareto | 66.89 | 29.44 | 0.440x | FAIL |
| aalto_grp_aov | 25.66 | 23.07 | 0.899x | PASS |
| aalto_grp_prior_mean | 31.19 | 34.34 | 1.101x | PASS |
| aalto_grp_prior_mean_var | 72.40 | 87.06 | 1.202x | PASS |
| aalto_lin | 32.16 | 31.10 | 0.967x | PASS |
| aalto_lin_std | 34.03 | 30.53 | 0.897x | PASS |
| aalto_lin_std_t | 55.97 | 34.39 | 0.614x | PASS |
| aalto_poisson_hurdle | 3187.70 | 863.35 | 0.271x | FAIL |
| aalto_poisson_simple | 846.75 | 230.26 | 0.272x | FAIL |

## Measured failing phases

| Model | Preparation (ms) | NUTS (ms) | Generated quantities/output (ms) |
|---|---:|---:|---:|
| aalto_gpareto | 32.60 | 14.95 | 13.10 |
| aalto_poisson_hurdle | 19.34 | 26.05 | 3134.75 |
| aalto_poisson_simple | 7.74 | 8.91 | 818.54 |

The two Poisson models use the generated-quantities interpreter; output dominates their runtime. Pareto spends more than half its measured internal time in source/data preparation. These measurements identify the next optimization targets; this change adds tests and does not claim to fix those costs.

The initial run, before phase instrumentation and matching file output, also failed the same three models (Pareto 0.449x, hurdle-Poisson 0.267x, simple Poisson 0.279x). The final run above is the authoritative measurement.

Correctness tests are registered in default CTest. Live performance is a separate target, so adding the oracle does not make routine CI depend on a CmdStan checkout or shared-runner timing. Run `cmake --build build-rel --target check_educational_performance` to enforce the speed floor. Its nonzero exit is expected on the measured candidate.

Raw CSV and stderr artifacts remain under the output directory named in the JSON report. The retained JSON carries all paired timing observations and source/reference identities. The import is 13 teaching fixtures, including seven synthetic datasets; these results do not establish full-size course-data performance or posterior convergence.
