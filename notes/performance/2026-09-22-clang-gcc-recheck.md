# Clang/GCC recheck, 2026-09-22

The user requested a quick fresh measurement before deciding whether to stop
using GCC. Keep GCC for Linux releases: the historical `normal_mixture`
slowdown persists, and Clang also increases preparation time on one brms
fixture. This experiment changes no production sources or compiler defaults.

## Setup and evidence

- Production base: `f1face395bfe9511a81fa2bb8c53decd3bb9b4a0`, fetched from
  `origin/HEAD`; experiment: `b1f78bfd695d12a11b0b2eb95ae80550d5d26a2c`.
  The experiment checks that runtime/compiler sources, CMake, dependency
  fetching, and the existing timing driver match the base.
- [Completed run and raw artifact](https://github.com/seantalts/stanli/actions/runs/35762081675),
  [paired observations and build metadata](2026-09-22-clang-gcc-evidence.json.gz),
  [summary CSV](2026-09-22-clang-gcc.csv).
- One GitHub Linux x86_64 runner, Intel Xeon Platinum 8370C, glibc 2.28,
  GCC 14.2.1 and Clang 21.1.8. Processes pinned to CPU 0; one compiler runs
  at a time; OpenMP/BLAS thread limits are one. The hosted VM can still have
  outside interference.
- Both runtime and benchmark use Release `-O3 -DNDEBUG -ffp-contract=off`
  and the current CMake compiler-specific policies. Clang's test-only `-O0`
  policy does not apply. Both use the same embedded stanc3 build and MIR.
- Six fresh-process pairs per model, alternating compiler order, with the
  unchanged `bench_grad --timed` driver: at least 200 ms warmup and 250 ms
  measurement. Six separate `--prep` pairs use the reverse order. GNU time
  records process peak RSS. Six Clang/Clang pairs on `normal_mixture` provide
  a same-binary timing control. No observations are discarded.
- Pins: stanc3 `d58446e631b02cacc5355e373defc6092a684554`; Stan Math
  `5252d51d47c1d5e78005fc043ad996fad6dd8da8`; posteriordb
  `28f8d3d6e975315f42aa274a8399f21e07a43b30`. Source, data, MIR, library,
  and benchmark hashes are in the evidence. The executable recipe lives in
  `tools/compiler_comparison_20260922.py` and
  `.github/workflows/compiler-comparison.yml` on the experiment branch.

## Warm gradients

Times are median microseconds. Ratio is the median of the six within-pair
Clang/GCC time ratios; below one favors Clang. MAD is median absolute
deviation, in ratio units; per-compiler MADs are in the CSV.

| Model | GCC us | Clang us | Clang/GCC | Ratio MAD |
| --- | ---: | ---: | ---: | ---: |
| normal_mixture | 101.164 | 118.489 | 1.1730 | 0.0039 |
| eight_schools_noncentered | 0.368 | 0.352 | 0.9583 | 0.0024 |
| garch11 | 13.547 | 12.288 | 0.9090 | 0.0176 |
| logistic_regression_rhs | 95.323 | 94.781 | 0.9896 | 0.0138 |
| sw_asymlaplace | 6.458 | 6.652 | 1.0297 | 0.0088 |
| s2_zi_asymlaplace | 7.184 | 7.282 | 1.0108 | 0.0113 |
| s2_gev | 13.171 | 13.406 | 1.0164 | 0.0100 |

Geometric mean ratio: **1.0097**. The same-binary control is **1.0025**,
MAD **0.0036**, with individual ratios 0.9769–1.0083. Small differences are
not a basis for choosing a compiler. Every `normal_mixture` pair is slower
with Clang, by 16.9–18.5%.

[PR #259](https://github.com/seantalts/stanli/pull/259) reported 17–18% on
`normal_mixture` in August. This rechecks that signal on current sources;
the other six models are a current selection of controls and the recently
tuned brms models, not a reconstruction of August's complete model set.

## Preparation, memory, and size

Preparation is existing MIR and JSON to a bound executor, excluding gradient
evaluation and process startup. Entries are median milliseconds.

| Model | GCC ms | Clang ms |
| --- | ---: | ---: |
| normal_mixture | 7.125 | 7.146 |
| eight_schools_noncentered | 0.538 | 0.590 |
| garch11 | 4.451 | 4.419 |
| logistic_regression_rhs | 14.656 | 14.792 |
| sw_asymlaplace | 2.657 | 2.689 |
| s2_zi_asymlaplace | 4.672 | 6.425 |
| s2_gev | 1.449 | 1.484 |

The six `s2_zi_asymlaplace` preparation pairs are all 36–38% slower with
Clang (about 1.75 ms). Eight-schools preparation also rises by about 0.05 ms.
One normal-mixture preparation observation is noisy; it remains in the data.

Clang's benchmark process uses 2.0–2.4 MiB less median peak RSS across the
seven models. This is whole-process peak memory from a statically linked
benchmark, not retained model memory or Python/R memory. The unstripped
shared library is 50.29 MiB with Clang versus 47.71 MiB with GCC (+5.4%).
Gzip-compressed libraries are 15.70 versus 16.15 MiB (-2.8%); these are not
wheel/download measurements.

## Numerical results and limits

Both compilers pass the existing CmdStan replay on all seven models and
three points: 23,625 values each. At every timed point, their log densities
and gradients match exactly (0 ULP). The three brms models use matching
Linux compiler references and remain within 4 ULP across the replay.

The other four models use the existing cross-platform scaled-error gate.
`logistic_regression_rhs` reaches 2,048 ULP against its macOS CmdStan
recording, with scaled error 6.55e-15, identically for both compilers. Passing
this replay is not a claim that all seven models meet a 10-ULP CmdStan bound.
No reference or tolerance was changed.

This is a bounded warm-gradient/preparation experiment. It does not measure
first gradients, complete sampling, cold compilation, installed interfaces,
Windows, or the whole corpus. Ccache was warm and differed between compilers,
so build durations are not a compiler-speed comparison.

The first experiment run stopped at a container Git checkout trust check
before timings. The corrected run above completed; no timing results were
selected between runs. Separately, [main's post-submit run](https://github.com/seantalts/stanli/actions/runs/35752295246)
has an unresolved `radon_pooled` vectorization timing failure and an Intel
Mac `kronecker_gp` numerical replay failure. Neither is fixed or explained
by this Linux compiler comparison.

Keep the current compiler policy. If compiler unification becomes a priority,
the next discriminating experiment is a matched profile of normal-mixture
gradients and zero-inflated asymmetric-Laplace preparation with both compilers.
Those causes remain untested; broader migration is not authorized by this
measurement request.
