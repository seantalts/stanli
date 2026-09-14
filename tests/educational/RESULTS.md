# Educational corpus results — 2026-09-14

**All 13 models pass correctness and the 0.5x end-to-end speed floor.** The
three-point CmdStan oracle checks 6,609 scalar values, with worst scaled error
8.30e-15. All sampling CSV and sampled-parameter mean checks pass.

Apple M3 Ultra, macOS arm64, Apple Clang 21, Release. CmdStan uses `--O1`,
`-O3`, and `-ffp-contract=off`. Each reported median contains three alternating
pairs following one warmup pair: 1000 warmup + 1000 saved draws, full 17-digit
CSV files for both engines. Stanli includes source compilation and all
preparation; the CmdStan executable is already compiled. Its build time is
recorded separately. No model, output, phase or performance failure is excluded.

The baseline is commit `7d4f25ec`; the optimized candidate adds general RNG
lowering, compiler procedure pruning, and buffered decimal formatting. See
[baseline observations](benchmark-results.json), [optimized observations](optimized-benchmark-results.json)
and the [investigation ledger](../../docs/superpowers/plans/2026-09-14-educational-performance.md).
These reports include binary hashes, toolchain identities, raw timings,
dispersion, phase measurements and posterior mean comparisons.

| Model | Baseline Stanli (ms) | Optimized Stanli (ms) | CmdStan (ms) | Speed vs CmdStan | Floor |
|---|---:|---:|---:|---:|---|
| aalto_bern | 21.02 | 17.63 | 16.97 | 0.962x | PASS |
| aalto_binom | 17.29 | 15.86 | 13.65 | 0.861x | PASS |
| aalto_binom2 | 20.21 | 18.39 | 15.82 | 0.860x | PASS |
| aalto_binomb | 18.86 | 16.37 | 12.77 | 0.780x | PASS |
| aalto_gpareto | 66.89 | 48.68 | 26.83 | 0.551x | PASS |
| aalto_grp_aov | 25.66 | 23.03 | 23.20 | 1.007x | PASS |
| aalto_grp_prior_mean | 31.19 | 28.82 | 35.04 | 1.216x | PASS |
| aalto_grp_prior_mean_var | 72.40 | 62.42 | 82.04 | 1.314x | PASS |
| aalto_lin | 32.16 | 27.17 | 31.68 | 1.166x | PASS |
| aalto_lin_std | 34.03 | 23.44 | 28.15 | 1.201x | PASS |
| aalto_lin_std_t | 55.97 | 29.52 | 33.95 | 1.150x | PASS |
| aalto_poisson_hurdle | 3187.70 | 665.37 | 865.01 | 1.300x | PASS |
| aalto_poisson_simple | 846.75 | 97.69 | 242.00 | 2.477x | PASS |

## Why the failing models improved

The Poisson models previously interpreted generated-quantity rejection loops.
Scalar integer RNG draws now run in the existing register machine, using the
same Stan Math RNG functions. Draw-dependent sizes still fall back, and
runtime indices still receive bounds checks. The Student-t teaching model
also benefits from the shared Student-t RNG kernel.

Pareto spent most of its time preparing the model. Model compilation now
skips optimization of unused backend procedures and unreachable function
bodies after inlining; the general function API retains its exported functions.
CSV formatting was another measured bottleneck. Bounded buffering and {fmt}
replace per-value printf conversion while retaining exactly the same 17-digit
CSV text, including the host's NaN spelling.

| Model | Preparation before → after (ms) | NUTS before → after (ms) | GQ/output before → after (ms) |
|---|---:|---:|---:|
| aalto_gpareto | 32.60 → 24.54 | 14.95 → 13.86 | 13.10 → 4.44 |
| aalto_poisson_hurdle | 19.34 → 39.58 | 26.05 → 28.55 | 3134.75 → 587.58 |
| aalto_poisson_simple | 7.74 → 8.43 | 8.91 → 8.22 | 818.54 → 70.29 |

Hurdle-Poisson pays more preparation time to compile its generated-quantity
program, but saves that cost within roughly eight output rows at these data
sizes. No runtime special case recognizes a teaching model or its source.

A separate [matched revision A/B](revision-ab-results.json) alternated the
original and optimized Stanli binaries with the same settings on the same
machine. **All 52 complete CSVs match bitwise** (13 models × four seeds).
Three timed pairs after one warmup pair measured the following improvements:

- aalto_gpareto: 1.35x faster.
- aalto_poisson_hurdle: 4.75x faster.
- aalto_poisson_simple: 8.51x faster.
- aalto_lin_std_t: 1.96x faster.

Correctness tests run in default CTest. The live performance gate is a separate
target because it requires CmdStan and an idle machine:
`cmake --build build-rel --target check_educational_performance`.

These measurements cover the 13 supplied teaching fixtures, seven of which
have synthetic data. They do not establish full-size course-data performance
or posterior convergence. Pareto has the smallest margin above the floor;
retain the full wall-time gate when changing compiler preparation costs.
