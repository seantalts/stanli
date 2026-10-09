# Vector math for fast mode: what it could buy

Status: measurement record. Spike of 2026-10-06 on `fastmath/mode` at
`73a344cd`; ceiling re-profiled on 2026-10-09 on `main` at `f99f3ec7`, after
the observation collapse
([note](2026-10-06-sufficient-statistic-collapse.md)) landed. Nothing here is
in the build. The spike's code is a local throwaway commit (`74744058`,
branch `spike/vector-libm`, not pushed); its text results and the profiling
scripts are under `data/2026-10-06-vector-libm-spike/`.

## Decision

Not built yet, and still worth building as fused kernels for the logit
densities, not as a libm swap.

- Elementary functions (`exp`, `log`, `log1p`) inside ops at least four
  elements wide are 10% of warm-gradient time on the median corpus model and
  over half on 34 models. The observation collapse did not change that
  (below), because the models that spend their time there have no repeated
  rows to collapse.
- A four-wide AVX2 version of two sites gave 1.18x in geometric mean over
  the 46 models that reach them and 1.2x to 1.7x on the 20 Bernoulli-logit
  models, with nothing lost on controls.
- It only pays where a fused density kernel replaces the Stan Math code. The
  existing `STANLI_PACKET_MATH` switch reaches constrain transforms,
  `seq_sum` and one `inv_logit`, about 1.5% of gradient time, and measured
  0.999x.

## Ceiling: share of gradient time in transcendentals

Sampling profiler over `bench_grad --timed --fast-math` (an `LD_PRELOAD`
shim, `sprof.c`, because `perf` is not available on the machine; 1999 Hz, 2 s
per model), each sample classified by the function it is in and by the width
of the op being run. Shares are fractions of each model's own gradient time,
unweighted across models.

| | 2026-10-06 | 2026-10-09, collapse off | 2026-10-09, collapse on |
| --- | ---: | ---: | ---: |
| models counted | 335 | 337 | 337 |
| all transcendental: mean / median / p90 | 0.279 / 0.243 / 0.651 | 0.282 / 0.250 / 0.654 | 0.295 / 0.261 / 0.656 |
| elementary: mean / median / p90 | 0.245 / 0.220 / 0.520 | 0.247 / 0.221 / 0.523 | 0.258 / 0.230 / 0.515 |
| elementary in ops of width >= 4: mean / median / p90 | 0.193 / 0.106 / 0.508 | 0.195 / 0.110 / 0.510 | 0.192 / 0.102 / 0.504 |
| models with that share >= 10% / 30% / 50% | 171 / 98 / 34 | 174 / 104 / 34 | 169 / 98 / 34 |

`log1p` (mean 0.078 with the collapse on) and `exp` (0.062, plus 0.030
through Eigen's scalar op) lead; `log` is 0.035 and `lgamma` 0.021.

By site, with the collapse on: `stan::math::bernoulli_logit_glm_lpmf` 0.020
(10 models at 5% or more, the `wells_*` models at 0.65 to 0.78),
`student_t` 0.014 (25 models), stanli's `bernoulli_logit` kernel 0.012 (the
IRT models at 0.64 to 0.70), the collapse's own `family_grouped_fwd` 0.011
(17 models), `binomial_logit_lpmf` 0.009, `neg_binomial_2_log_glm_lpmf`
0.008. The 25 models with the largest wide-elementary share are within 0.08
of their collapse-off share, every one.

Eleven models moved by more than 0.10 with the collapse on. Nine fell:
`ch12_m12_5` 0.46 to 0.02, `sw_lognormal` 0.39 to 0.02, `nes_logit_model`
0.77 to 0.43, `aalto_poisson_simple` 0.30 to 0.00, `ch12_m12_7`,
`ch16_m16_1`, `i319_pois_re`, `dogs`, `i319_pois_fixed`. Two rose because
what is left after collapsing is mostly transcendental: `ch12_m12_4` 0.02 to
0.55 and `dogs_hierarchical` 0.02 to 0.35. A collapsed model's share is a
share of much less time (`nes_logit_model` 13.7 us to 0.13 us), so these are
not where vector math would be felt.

What the spike's own estimate from these shares was: a 3x to 4x faster
function is about 1.08x on the median model and 1.5x to 2x in the top
decile. That is an estimate, not a measurement.

## Measured: four-wide AVX2 at two sites

`bernoulli_logit` and `bernoulli_logit_glm` (one `exp` and one `log1p` per
element) and `student_t`'s `log1p`, behind a run-time switch, in two
versions: self-contained polynomial kernels, and glibc's `libmvec`. Paired
A/B on one P-core, 9 rounds, the machine idle (`2026-10-06/ab2_report.txt`;
identical-binary control 0.9997).

| group | self-contained | libmvec |
| --- | ---: | ---: |
| 46 models taking the vector path, geomean | 1.18x | 1.33x |
| the Bernoulli-logit models among them | 1.2x to 1.7x | 1.4x to 2.2x |
| 30 controls, geomean | 0.995x | 0.991x |

- `student_t`: three models gain 1.10x to 1.14x; the other 23 are flat,
  because brms mostly uses it for scalar priors.
- Size +20.5 KB stripped; preparation time unchanged
  (`2026-10-06/prep_time.txt`).
- The self-contained kernels divide, which makes them about 1.5x slower than
  libmvec on P-cores. A production version should be table and polynomial.
- `libmvec` is not a candidate to ship: glibc only, different by glibc
  version, and no `lgamma`.

### Accuracy

- Kernel level (`2026-10-06/vmath_test_2M.txt`, `vk_test.txt`): the
  self-contained `log1p` and `exp` are within 1 ULP of glibc's scalar
  functions over 4 million arguments per range; libmvec within 3. One
  exception, not located: over `[-745, 709]` the self-contained `exp`
  differs from glibc without bound at some argument, probably at the
  underflow edge, where one gives 0 or infinity and the other does not.
- Model level (`2026-10-06/values_cmp_vs_base.txt`): against the baseline at
  the corpus points, 20 models change with the self-contained kernels; the
  worst gradient difference scaled by the largest entry is 7.3e-16 and the
  worst log density difference 2.2e-14 (`hier_2pl`, 163 ULP). With libmvec 22
  models change and the worst scaled gradient difference is 3.4e-15. No
  finite or non-finite mismatch.

## Other findings

- `fma()` from partial evaluation is a libm call on baseline x86-64, which
  has no FMA instruction: 1.5% of gradient time on average in the 2026-10-06
  profile and up to 25% on the `logmesquite` models. On 2026-10-09 it is
  1.2% with the collapse off and 0.5% with it on (7 models at 5% or more,
  most 0.10).
- The spike's rebuilt binary read 0.88x to 0.91x on `ch09_m9_1`, a control
  that runs none of the changed code, in every arrangement tried. The cause
  was not found; code placement is the usual suspect on models that small.

## Limits

- The spike ran no CTest, no CmdStan corpus replay, no model-level
  high-precision reference, and only clang on one Linux AVX2 machine.
- Since the spike, #443 gave the collapse's family kernel one `exp` and one
  `log1p` per group for the logit forms; a vector version of that kernel was
  not tried.
- The profiler crashes intermittently when preloaded on a few models
  (`sw_cumulative_cs`, `cm_invgaussian`, `s2_hurdle_cumulative`); they are
  missing from one or both 2026-10-09 runs. Three models have a non-finite
  benchmark point and are never timed.
- The width hook costs 8% to 9% on the smallest models and about 1% on
  larger ones; it is in the profiled binary only.

## Reproducing the profile

`data/2026-10-06-vector-libm-spike/`: `sprof.c` (build as a shared object),
`prep_corpus.py`, `analyze_prof.py`, `summarize_prof.py`. The width
breakdown needs the three `STANLI_SPIKE_PROFILE` hunks from commit
`74744058` in `runtime/src/executor.cpp` and `-gline-tables-only`.
