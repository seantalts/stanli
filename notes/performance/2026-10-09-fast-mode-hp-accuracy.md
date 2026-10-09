# Fast mode against a high-precision reference

Measured 2026-10-09 on macOS arm64 (clang), `fastmath/hp-reference` on
`origin/main` 62375382, which includes the observation collapse (#443, #449). The
tools are in [`tools/hp/`](../../tools/hp/README.md). Context: [numerics versus
speed](2026-10-05-numerics-vs-speed.md).

## Summary

- 344 of 352 corpus models were evaluated at 80 digits at their recorded
  points (1,029 points). 328 pass the validity gate, 13 fail it and 3 are
  ill-conditioned; the 16 are listed with causes below. Every one of them has
  a cause outside fast mode: Stan Math's approximate normal-CDF gradients, a
  bernoulli_logit_glm cutoff, double-precision conditioning, or a gradient
  that is undefined at the point. The three arms (CmdStan, stanli default,
  stanli fast) are at the same distance from the reference on them, to
  within a small factor.
- On the 328 valid models fast mode is not less accurate than default in any
  distribution statistic. Log density error, median / p90 / max: default
  2.9e-16 / 3.5e-15 / 2.0e-13, fast 2.7e-16 / 1.9e-15 / 2.0e-13. Gradient
  error: default 4.8e-16 / 4.9e-15 / 3.0e-13, fast 3.8e-16 / 3.1e-15 / 3.0e-13.
  The maxima are the same models in both modes (`hier_2pl` log density,
  `hmm_example` gradient), so they are double-precision error, not fast mode.
- Rule for "materially less accurate": fast error more than 2x default error
  and above 1e-14. One model meets it: `ch14_m14_11`, gradient 3.5e-15 to
  3.7e-14 (CSE merge). Fast mode is more accurate than default on 4 models in
  log density and 8 in gradient, by up to 500x, all through the
  observation collapse.
- Fast mode returns bit-identical values to default on 188 of 344 models and
  differs on 156; where it differs the gate-metric distance between the two
  is at most 1.6e-13 (`election88_full`, where fast is the more accurate).
  Fast and default agree on every rejection.

## Method

The reference is an mpmath interpreter over the MIR from `stanc --O0
--debug-optimized-mir`, at 80 digits, evaluating `log_prob_propto_jacobian`
with the same term dropping as the pinned Stan Math. Parameters are the recorded
unconstrained points 0, 1 and 2 of `tools/verify_refs.py`. Data are the
doubles stanli reads; transformed data are recomputed at 80 digits.

Gradients. Up to 300 unconstrained parameters (315 models) the reference
gradient is a central finite difference per coordinate with step 1e-26, whose
truncation error is about 1e-52. Above 300 (29 models) it is four directional
derivatives per point along unit vectors drawn from
`random.Random("MODEL:POINT:K")` (K = 0..3, seeds recorded in the result
files), each a central difference with two evaluations. A rerun at 120
digits gave identical arm errors on the models that failed the gate, and a
different step size agreed to 1e-20. At a support boundary, where one side of
the difference is outside the support, the difference is one-sided.

Errors, using the fast gate's definitions
([TESTING.md](../../TESTING.md#fast-mode)).

- Log density: `|a - b| / max(|a|, |b|, 1)`.
- Full gradient: `max|g - r| / max(1, max|g|, max|r|)` over the entries.
- Directional: `|g.v - D| / max(1, |g_cmdstan|_2)`, where `g` is the arm's
  gradient, `v` a unit direction and `D` the reference directional
  derivative.

Arms: the recorded CmdStan values, `build-rel/stanli_check` and the same with
`--fast-math`. A point CmdStan rejects is not scored (it is a rejection
parity check; `s2_invgaussian` at all three points, `s2_nlf` at two,
`lupdf-inlining` at one).

Validity gate: a model is used for conclusions only if its CmdStan and
default errors are all below 1e-12. Otherwise it is `ill_conditioned` when it
is in `verify_refs.ILL_CONDITIONED` and `gate_fail` if not. Some points are not scored. `dogs_log` points 0 and 1 have a log density of
-inf (outside the uniform support), so only point 2 is scored, with a
one-sided difference for a parameter at the boundary. Three gradient points
are excluded because the reference gradient is undefined there: `s2_com_poisson` points 1 and 2 (the model branches on `nu == 1`, which
holds exactly at those points, so autodiff gives 0 for `shape`), and
`kronecker_gp` point 2 (435 of 438 CmdStan gradients are non-finite).

Comparison: `fast_worse` when fast error is above twice the default error
and above 1e-14; `fast_better` is the mirror image; otherwise `similar`.
Each model's error is the maximum over its scored points.

Attribution reruns `--fast-math` on the stored reference with one pass off
at a time (`STANLI_NO_COLLAPSE`, `_CSE`, `_PARTITION`, `_REROLL`,
`_FUSED_DENSITY`, `_GP_DIAGONAL_FUSION`, `_ISLAND`) and with the stanc3
partial evaluation separated from the runtime passes through a local,
uncommitted switch in `stanli_check`. A pass is called the driver when
disabling it returns exactly the default values. Partial evaluation alone
(runtime passes off) matches default exactly on 108 of the 156 differing
models.

## Coverage

352 models in the corpus, 344 scored, 8 not.

| model | reason |
|---|---|
| cm_ddm | Unsupported: density wiener |
| lotka_volterra | Unsupported: fn integrate_ode_rk45 |
| mother | Unsupported: fn algebra_solver |
| nn_rbm1bJ100 | timeout |
| one_comp_mm_elim_abs | Unsupported: fn integrate_ode_bdf |
| s2_invgaussian | no scored point |
| s2_wiener | Unsupported: density wiener |
| soil_incubation | Unsupported: fn integrate_ode_rk45 |

Gradient reference: directional 29 models, fd 315 models.

## Validity gate

gate_fail 13, ill_conditioned 3, valid 328.

| model | n | status | cmdstan lp | cmdstan grad | default lp | default grad | fast lp | fast grad | lp dps diff | cause |
|---|---|---|---|---|---|---|---|---|---|---|
| cm_lba1 | 7 | gate_fail | 7.63e-16 | 4.56e-10 | 7.63e-16 | 4.56e-10 | 7.63e-16 | 4.56e-10 | 2.89e-80 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| hmm_drive_0 | 6 | gate_fail | 5.09e-15 | 1.02e-11 | 5.23e-15 | 1.02e-11 | 5.23e-15 | 1.02e-11 | 7.10e-80 | forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms |
| hmm_drive_1 | 6 | gate_fail | 7.81e-15 | 2.62e-11 | 7.81e-15 | 2.62e-11 | 7.81e-15 | 2.62e-11 | 2.63e-80 | forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms |
| hmm_gaussian | 14 | gate_fail | 8.11e-15 | 4.42e-11 | 8.11e-15 | 4.42e-11 | 8.11e-15 | 4.42e-11 | 3.42e-80 | forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms |
| i320_gp_expquad | 34 | ill_conditioned | 1.26e-11 | 1.48e-10 | 1.26e-11 | 1.48e-10 | 1.26e-11 | 1.48e-10 | 8.51e-77 | Cholesky pivot 3.7e-12; reference converged (80 against 160 digits) |
| iohmm_reg | 29 | gate_fail | 1.26e-15 | 4.12e-11 | 1.26e-15 | 4.12e-11 | 1.26e-15 | 4.12e-11 | 1.12e-80 | forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms |
| kronecker_gp | 438 | gate_fail | 1.21e-15 | 5.10e-05 | 1.21e-15 | 5.10e-05 | 1.21e-15 | 5.10e-05 | 6.86e-81 | eigendecomposition of repeated eigenvalues; 435 of 438 gradients non-finite in CmdStan at point 2 (excluded there) |
| logistic_regression_rhs | 3075 | gate_fail | 1.35e-12 | 4.14e-12 | 1.35e-12 | 4.14e-12 | 1.35e-12 | 4.14e-12 | 1.34e-82 | bernoulli_logit_glm drops log1p(exp(eta)) for eta below -20, up to 2e-9 per observation; mirroring the cutoff brings the log density to 1e-14 |
| s2_cens_interval | 3 | gate_fail | 4.49e-16 | 2.59e-05 | 4.49e-16 | 2.59e-05 | 1.60e-16 | 2.59e-05 | 5.89e-79 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| s2_cumulative_probit | 4 | gate_fail | 1.68e-16 | 2.19e-06 | 1.68e-16 | 2.19e-06 | 1.68e-16 | 2.19e-06 | 2.72e-81 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| s2_gp_by_gr | 52 | ill_conditioned | 8.94e-10 | 4.28e-08 | 8.94e-10 | 4.28e-08 | 8.94e-10 | 4.28e-08 | 4.92e-75 | Cholesky pivot 1.8e-12; reference converged |
| s2_mi_trunc_lb | 9 | gate_fail | 2.35e-16 | 2.53e-07 | 1.18e-16 | 2.53e-07 | 3.53e-16 | 2.53e-07 | 1.33e-81 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| s2_weights_trunc | 3 | gate_fail | 4.27e-16 | 2.82e-07 | 4.27e-16 | 2.82e-07 | 4.27e-16 | 2.82e-07 | 3.42e-81 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| sw_cens | 3 | gate_fail | 1.81e-16 | 5.80e-07 | 1.81e-16 | 5.80e-07 | 1.81e-16 | 5.80e-07 | 1.46e-82 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |
| sw_gp | 44 | ill_conditioned | 8.09e-11 | 4.75e-09 | 8.09e-11 | 4.75e-09 | 8.09e-11 | 4.75e-09 | 3.79e-76 | Cholesky pivot 1.1e-12; reference converged |
| sw_trunc | 3 | gate_fail | 1.67e-16 | 6.76e-07 | 1.67e-16 | 6.76e-07 | 1.67e-16 | 6.76e-07 | 2.68e-81 | Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate (relative error 2e-7 to 7e-7 at sample arguments); the three arms agree |

## Error distribution over 328 valid models (max over points)

| arm | quantity | median | p90 | p99 | max |
|---|---|---|---|---|---|
| cmdstan | lp | 3.08e-16 | 6.99e-15 | 9.70e-14 | 2.00e-13 |
| cmdstan | grad | 5.33e-16 | 5.12e-15 | 2.83e-14 | 3.01e-13 |
| default | lp | 2.94e-16 | 3.52e-15 | 1.01e-13 | 2.00e-13 |
| default | grad | 4.83e-16 | 4.89e-15 | 2.66e-14 | 3.01e-13 |
| fast | lp | 2.65e-16 | 1.94e-15 | 5.21e-14 | 2.00e-13 |
| fast | grad | 3.78e-16 | 3.11e-15 | 3.66e-14 | 3.01e-13 |

## Fast against default

| quantity | fast worse | fast better | similar |
|---|---|---|---|
| lp | 0 | 4 | 324 |
| grad | 1 | 8 | 319 |

Rule: fast_worse when fast error > 2x default error and > 1e-14; fast_better is the mirror.

### Fast less accurate than default (1 models)

| model | n | default lp | fast lp | default grad | fast grad |
|---|---|---|---|---|---|
| ch14_m14_11 | 5 | 1.68e-15 | 1.68e-15 | 3.52e-15 | 3.66e-14 |

### Fast more accurate than default (8 models)

| model | n | default lp | fast lp | default grad | fast grad |
|---|---|---|---|---|---|
| election88_full | 90 | 1.64e-13 | 5.25e-15 | 1.52e-14 | 1.26e-15 |
| ch12_m12_4 | 6 | 6.51e-14 | 1.25e-16 | 2.24e-14 | 2.14e-16 |
| ch12_m12_7 | 10 | 2.63e-14 | 8.71e-16 | 2.24e-14 | 3.56e-16 |
| ch12_m12_5 | 11 | 1.99e-14 | 2.49e-16 | 2.24e-14 | 2.14e-16 |
| M0_model | 2 | 7.45e-15 | 1.15e-16 | 2.24e-14 | 9.22e-16 |
| ch12_m12_3_alt | 2 | 2.31e-15 | 1.23e-16 | 1.71e-14 | 2.36e-16 |
| radon_pooled | 3 | 1.99e-15 | 1.53e-16 | 1.26e-14 | 2.48e-16 |
| Mt_model | 4 | 5.97e-15 | 1.71e-16 | 1.20e-14 | 3.20e-16 |

### Fast above default by more than 1e-15 (top 12 by increase)

| model | n | quantity | default | fast |
|---|---|---|---|---|
| ch14_m14_11 | 5 | grad | 3.52e-15 | 3.66e-14 |
| nes | 10 | lp | 6.08e-16 | 2.79e-15 |
| nes | 10 | grad | 1.28e-15 | 2.95e-15 |
| kilpisjarvi | 3 | lp | 1.67e-16 | 1.81e-15 |
| normal_mixture_k | 14 | grad | 1.52e-14 | 1.67e-14 |
| dogs | 3 | lp | 4.37e-16 | 1.90e-15 |
| s2_gev | 4 | grad | 6.89e-16 | 2.11e-15 |
| blr | 6 | grad | 3.66e-16 | 1.61e-15 |

## Fast against default, all scored models

344 scored models; fast returns bit-identical values to default on 188 and differs on 156. Gate-metric distance between the two over the differing models: median 6.53e-16, p90 5.69e-15, max 1.59e-13.

Fast-mode feature whose removal gives back default's values exactly:

| feature | models |
|---|---|
| collapse | 53 |
| cse | 26 |
| partial evaluation | 21 |
| collapse (low bits differ from default) | 17 |
| several: collapse, cse | 14 |
| several: collapse, reroll | 9 |
| cse (low bits differ from default) | 3 |
| several: collapse, cse, reroll | 3 |
| several: cse, island | 3 |
| reroll (low bits differ from default) | 2 |
| island (low bits differ from default) | 1 |
| several: collapse, cse, partition, reroll, fused_density, gp_diagonal_fusion, island | 1 |
| partition | 1 |
| unresolved | 1 |

### Largest fast-to-default distances

| model | n | distance | default error (lp, grad) | fast error (lp, grad) | driver |
|---|---|---|---|---|---|
| election88_full | 90 | 1.59e-13 | 1.64e-13, 1.52e-14 | 5.25e-15, 1.26e-15 | collapse (low bits differ from default) |
| ch12_m12_4 | 6 | 6.50e-14 | 6.51e-14, 2.24e-14 | 1.25e-16, 2.14e-16 | collapse (low bits differ from default) |
| ch14_m14_11 | 5 | 3.31e-14 | 1.68e-15, 3.52e-15 | 1.68e-15, 3.66e-14 | cse (low bits differ from default) |
| normal_mixture_k | 14 | 3.19e-14 | 2.01e-15, 1.52e-14 | 2.01e-15, 1.67e-14 | cse |
| ch12_m12_7 | 10 | 2.60e-14 | 2.63e-14, 2.24e-14 | 8.71e-16, 3.56e-16 | collapse |
| ch12_m12_5 | 11 | 2.22e-14 | 1.99e-14, 2.24e-14 | 2.49e-16, 2.14e-16 | collapse |
| M0_model | 2 | 2.16e-14 | 7.45e-15, 2.24e-14 | 1.15e-16, 9.22e-16 | several: collapse, cse |
| ch12_m12_3_alt | 2 | 1.70e-14 | 2.31e-15, 1.71e-14 | 1.23e-16, 2.36e-16 | several: collapse, cse |
| radon_pooled | 3 | 1.26e-14 | 1.99e-15, 1.26e-14 | 1.53e-16, 2.48e-16 | collapse |
| Mt_model | 4 | 1.20e-14 | 5.97e-15, 1.20e-14 | 1.71e-16, 3.20e-16 | several: collapse, cse |
| ch12_m12_3 | 2 | 9.22e-15 | 4.14e-15, 9.10e-15 | 1.23e-16, 2.36e-16 | several: collapse, cse |
| aalto_poisson_hurdle | 2 | 8.30e-15 | 3.92e-15, 8.30e-15 | 2.02e-16, 1.89e-16 | collapse (low bits differ from default) |

## Rejection parity breaks: 0 models

| model | points differing |
|---|---|


## Worst cases and causes

Fast mode:

- `ch14_m14_11` (the one `fast_worse` model, gradient 3.5e-15 to 3.7e-14
  at point 2): disabling CSE returns default's error.
- `normal_mixture_k` (gradient 1.5e-14 to 1.7e-14): also CSE.
- `election88_full`, `ch12_m12_4`, `ch12_m12_5`, `ch12_m12_7`, `radon_pooled`,
  `M0_model`, `Mt_model`: fast is more accurate. The collapse replaces a sum
  over observations by grouped statistics, so the rounding error of the long
  sum is gone (`election88_full` log density 1.6e-13 to 5.3e-15).
- The driver table above gives the pass behind every other difference
  between fast and default.

Not fast mode (the same error in all three arms):

- Stan Math's `normal_lcdf`, `normal_lccdf` and `std_normal_lcdf` gradients
  are approximations: a one-function test model at sample arguments, compared
  with mpmath derivatives, shows relative errors of 1.7e-7 to 7e-7. Seven models fail the gate through them: `cm_lba1`,
  `s2_cens_interval`, `s2_cumulative_probit`, `s2_mi_trunc_lb`,
  `s2_weights_trunc`, `sw_cens`, `sw_trunc`. The student-t CDFs are exact.
- `logistic_regression_rhs` (log density 1.35e-12 at point 0, 1.8e-14 and
  3.7e-14 at points 1 and 2): `bernoulli_logit_glm_lpmf` returns `eta` instead
  of `-log1p(exp(-eta))` for `eta < -20`
  (`bernoulli_logit_glm_lpmf.hpp` line 116 to 121), which drops up to 2.1e-9
  per observation. Applying the cutoff in the reference leaves 1.6e-11 absolute at point 0
  (about 1e-14 scaled).
- `hmm_drive_0`, `hmm_drive_1`, `hmm_gaussian`, `iohmm_reg`: gradient error
  6.6e-13 to 4.4e-11, scaled by the largest entry (1e4 to 1.7e5), equal in
  all arms to the printed digits. The reference is converged in digits and in
  step, so this is the rounding error of the double-precision forward
  recursion.
- `i320_gp_expquad`, `s2_gp_by_gr`, `sw_gp`: Cholesky of a jittered
  exp-quad covariance with pivots near 1e-12. The reference log density agrees
  between 80 and 160 digits to 1e-75, so raising the digits does not change
  them; they have no transformed data, so the reference sees the same inputs
  as the doubles. Point 0 errors are 1.3e-11, 8.9e-10 and 8.1e-11 in log density and 1.5e-10,
  4.3e-8 and 4.8e-9 in gradient.
- `kronecker_gp` (gradients 3e-6 and 5e-5 at points 0 and 1): eigenvectors of
  a nearly degenerate covariance (TESTING.md, Known limits).
- `s2_com_poisson`, `dogs_log` and `kronecker_gp` point 2: see Method.

## Coverage gaps

`cm_ddm` and `s2_wiener` (wiener first passage), `lotka_volterra`,
`soil_incubation` (`integrate_ode_rk45`), `one_comp_mm_elim_abs`
(`integrate_ode_bdf`) and `mother` (`algebra_solver`) have no reference
implementation; the ODE solvers and the wiener density would need an
arbitrary-precision integrator or series with its own error control.
`nn_rbm1bJ100` exceeded the 1,800 s per-model timeout.
`s2_invgaussian` is evaluated but CmdStan rejects all three points, so it has
nothing to score.

## Reproducing

`tools/hp/README.md` has the commands. The reference runs took 0.7 CPU-hours
across the 344 models that finished, run 6 at a time. The result files are
not checked in.

## Limits

- One platform (macOS arm64) and one compiler; the CmdStan references are the
  platform's own replay. The arm errors include each platform's libm.
- Errors are at three points per model, all near the origin of the
  unconstrained space; they say little about a posterior's tails.
- The attribution switch for partial evaluation exists only as a local patch
  to `stanli_check`, which is not committed.
