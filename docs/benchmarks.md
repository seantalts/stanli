# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling it reuses a prepared autodiff graph and batches
independent work. Both shorten the first fit and every later gradient, by an
amount that depends on the model.

The current comparison covers **<!--gen:benchmark_models-->343<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->337<!--/gen--> accepted gradient
comparisons, the median model's gradient is **<!--gen:corpus_median-->2.14x<!--/gen--> faster**
in Stanli than in CmdStan. The tables cover every model, with incomplete
results listed separately.

## How we measure

Both engines evaluate the same model, data and unconstrained parameter vector
with the sampling log density (`propto=true`) and the Jacobian adjustment.
Coordinate `i` of the parameter vector is `0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`.

### Gradient time

Each engine builds its model and checks the point outside
the timed window, warms up for at least 200 ms, then measures batches of
gradients for at least 250 ms. Each measurement is a fresh process, and only
one engine runs at a time. Six pairs alternate the order (Stanli first, then
CmdStan first). The reported speedup is the median of the six within-pair
CmdStan/Stanli time ratios, so above 1x means Stanli is faster. The artifacts
keep the median absolute deviation (MAD) of the ratios and of each engine's
latency; read a small difference with that dispersion. The drivers share
[`benchmark_timer.hpp`](../tools/benchmark_timer.hpp).

### Numerical gate

Every accepted pair must agree on the log density and
every gradient component within scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Non-finite values, unequal widths
and failed commands produce no accepted pair. The independent
[three-point corpus replay](../TESTING.md#comparison-with-cmdstan-on-complete-models)
checks correctness separately.

### Setup

Stanli setup is the source-to-MIR compiler process plus the median
of six fresh-process preparations from MIR and JSON data to a bound executor.
CmdStan setup is stanc translation plus a fresh C++ build of the ordinary
model. Common CmdStan dependencies and precompiled headers are built in
advance. CmdStan data loading and model initialization are not counted, and
neither is the gradient driver's own build. Each compilation phase is timed
once per model with normal filesystem caches, so this is not a cold-cache or
in-process Python/R first-fit time. CmdStan builds finish before any gradient
timing starts. The timed stanc and make steps run one at a time with nothing
else building; only the gradient driver builds, whose time is not published, run
`--build-jobs` at a time. A model whose CmdStan inputs match an earlier build reuses that
build's stanc and make durations from the [build cache](#reproduction); the
event is marked `cached` with the run that measured it.

### Total sampling estimate

Each engine's setup time plus 2,000 times its
median warm gradient time. No sampler runs. Adaptation, tree building,
generated quantities and CSV output are left out, and a fixed gradient budget
says nothing about how long each engine needs to reach the same inference
quality. The run manifests record a 20,000-gradient budget; the tables
recompute totals for 2,000 gradients from the same measurements. A missing
setup component leaves the estimate blank and keeps the gradient result.

Six default-run models fail the gate and appear in the incomplete-results
table: `dogs_log`, `s2_invgaussian` and `sir` return non-finite values;
`kronecker_gp`, `sw_cens` and `s2_mi_trunc_lb` have scaled errors of
`1.13e-4`, `2.44e-6` and `2.99e-7`. The loop-vectorization table also rejects
`s2_gp_by_gr` at `5.33e-8`. [TESTING.md](../TESTING.md#comparison-with-cmdstan-on-complete-models)
lists the numerical exceptions.

## Why some models are faster, and some slower

### Less setup and repeated bookkeeping

Stanli does not compile each model
to C++. It binds data-dependent structure once and reuses its reverse plan and
working buffers. [How it works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)
explains what is prepared once and what each evaluation repeats.

### Independent observations give more work to batch

Regressions, GLMs and
IRT models can turn repeated scalar work into vector operations, which pays
dispatch once per vector. See [loop recovery](how-it-works.md#recovering-vector-operations-from-scalar-loops)
and the [optimization details](../runtime/src/OPTIMIZATIONS.md).

### Shared kernels and sequential work leave less to remove

Dense linear
algebra and ODEs spend most of their time in Stan Math in both engines.
Recurrences limit batching, and fallback derivatives keep local autodiff
costs. These models land near parity or run slower in Stanli; the table shows
which.

<a id="full-corpus"></a>

## Full corpus results

Every application model uses the same settings. Language-conformance fixtures
belong to the [numerical corpus](corpus-status.md) and are not timed here.
CmdStan uses its default compiler settings. The `CmdStan / Stanli` column is
how many times faster Stanli's gradient is; complete results are sorted by it,
fastest first. Incomplete results follow each table.

<!--gen:benchmark_catalog-->
Run `25ec9a23201be97b` (2026-10-19): 343 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 343 of 343 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 32.7x | 0.157 | 7.61 |
| `gpcm_latent_reg_irt` | 12.6x | 0.337 | 8.12 |
| `grsm_latent_reg_irt` | 11.9x | 0.177 | 6.75 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 0.0917 | 3.57 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 0.0951 | 3.81 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.2x | 0.143 | 4.04 |
| `radon_county_intercept` | 10.2x | 0.142 | 3.89 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.84x | 0.145 | 4.21 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.82x | 0.148 | 4 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.82x | 0.145 | 4.19 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.31x | 0.16 | 4.16 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.31x | 0.217 | 4.64 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.16x | 0.215 | 4.44 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 8.91x | 0.167 | 4.39 |
| `radon_pooled` | 8.29x | 0.0934 | 3.43 |
| `dogs` | 7.99x | 0.0421 | 3.76 |
| `aalto_poisson_hurdle` | 7.81x | 0.0411 | 3.06 |
| `s2_custom_vreal` | 7.73x | 0.00958 | 3.51 |
| `rats_model` | 7.45x | 0.0107 | 3.1 |
| `arK` | 7.24x | 0.0125 | 2.9 |
| `ch12_m12_3` | 6.73x | 0.014 | 2.53 |
| `ch12_m12_3_alt` | 6.61x | 0.0139 | 2.56 |
| `ch16_m16_1` | 6.54x | 0.0206 | 3.07 |
| `M0_model` | 6.29x | 0.0149 | 2.74 |
| `Mt_model` | 5.72x | 0.017 | 3.48 |
| `sw_nonlinear` | 5.36x | 0.0105 | 3.9 |
| `ctsem_ctsm_400` | 5.13x | 8.43 | 128 |
| `ctsem_ctsm` | 4.98x | 1.47 | 92.1 |
| `ch09_m5_8s` | 4.85x | 0.00864 | 2.88 |
| `ch14_m14_5` | 4.66x | 0.0119 | 2.89 |
| `s2_cumulative_cauchit` | 4.63x | 0.0155 | 3.7 |
| `ch09_m5_8s2` | 4.59x | 0.00828 | 2.87 |
| `ch15_m15_1` | 4.41x | 0.00938 | 3.2 |
| `logmesquite` | 4.3x | 0.0099 | 3.17 |
| `logmesquite_logvas` | 4.26x | 0.00942 | 3.38 |
| `mesquite` | 4.25x | 0.00949 | 3.08 |
| `logmesquite_logvash` | 4.24x | 0.00915 | 3.35 |
| `s2_nl_noloop` | 4.21x | 0.00967 | 3.93 |
| `logmesquite_logvolume` | 4.14x | 0.00781 | 3.03 |
| `election88_full` | 4.05x | 0.486 | 6.08 |
| `s2_logistic_normal` | 4.02x | 0.0275 | 6.35 |
| `ch15_m15_2` | 4.01x | 0.01 | 3.43 |
| `s2_car_icar` | 3.99x | 0.0119 | 4.41 |
| `nes` | 3.99x | 0.0484 | 3.38 |
| `logmesquite_logva` | 3.99x | 0.0086 | 3.16 |
| `Mth_model` | 3.88x | 0.0691 | 4.49 |
| `sw_spline_s` | 3.84x | 0.0112 | 4.48 |
| `sw_me` | 3.8x | 0.0112 | 4.15 |
| `ch14_m14_4` | 3.76x | 0.0102 | 2.8 |
| `ch14_m14_4x` | 3.76x | 0.0107 | 2.8 |
| `sw_dist_sigma` | 3.74x | 0.0126 | 3.86 |
| `ch09_m9_1_chains4` | 3.74x | 0.00825 | 3.11 |
| `logearn_logheight_male` | 3.72x | 0.0185 | 2.98 |
| `s2_s_cc` | 3.67x | 0.00978 | 4.31 |
| `s2_dist_sigma_re` | 3.65x | 0.0123 | 4.92 |
| `s2_me2_nomecor` | 3.64x | 0.0143 | 4.43 |
| `kidscore_interaction_c2` | 3.62x | 0.0125 | 3.05 |
| `s2_nlf` | 3.61x | 0.0107 | 3.99 |
| `kidscore_interaction` | 3.54x | 0.0131 | 3.06 |
| `sw_weights` | 3.54x | 0.0102 | 3.54 |
| `ch09_m9_1` | 3.53x | 0.00916 | 3.11 |
| `kidscore_interaction_c` | 3.51x | 0.0134 | 3.06 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.45x | 0.0232 | 4.91 |
| `s2_mm_weights` | 3.44x | 0.0134 | 4.68 |
| `logearn_height_male` | 3.43x | 0.0183 | 2.96 |
| `s2_mm` | 3.42x | 0.0126 | 4.68 |
| `kidscore_mom_work` | 3.42x | 0.014 | 2.96 |
| `kidscore_interaction_z` | 3.41x | 0.0131 | 3.09 |
| `kilpisjarvi` | 3.39x | 0.00786 | 2.82 |
| `logearn_interaction_z` | 3.38x | 0.0234 | 3.18 |
| `s2_gp_approx` | 3.36x | 0.0136 | 5.21 |
| `GLMM1_model` | 3.35x | 0.0317 | 3.33 |
| `sw_spline_t2` | 3.35x | 0.0152 | 4.93 |
| `sw_se` | 3.35x | 0.00899 | 3.55 |
| `s2_cox_cens` | 3.35x | 0.0144 | 5.09 |
| `ch16_m16_4` | 3.35x | 0.0108 | 2.91 |
| `s2_car` | 3.33x | 0.0138 | 4.4 |
| `logearn_interaction` | 3.32x | 0.024 | 3.08 |
| `aalto_grp_aov` | 3.29x | 0.00751 | 3.08 |
| `sw_lognormal` | 3.28x | 0.00937 | 3.71 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.26x | 0.00841 | 3.29 |
| `s2_s_by` | 3.25x | 0.0229 | 5.9 |
| `s2_car_esicar` | 3.23x | 0.0141 | 4.49 |
| `s2_gr_by` | 3.23x | 0.0123 | 4.95 |
| `s2_shifted_lognormal` | 3.22x | 0.0105 | 3.92 |
| `s2_gp_by_approx` | 3.21x | 0.0196 | 6.03 |
| `i319_gauss_re` | 3.19x | 0.0146 | 4.57 |
| `ch15_m15_8` | 3.19x | 0.0132 | 2.88 |
| `kidscore_momhsiq` | 3.18x | 0.0112 | 2.96 |
| `ch15_m15_9` | 3.17x | 0.0142 | 3.02 |
| `sw_acat` | 3.11x | 0.111 | 4.17 |
| `s2_t2_by` | 3.1x | 0.0204 | 5.83 |
| `radon_county` | 3.08x | 0.0643 | 3.3 |
| `ch15_m15_5` | 3.08x | 0.00986 | 3.24 |
| `s2_cox` | 3.01x | 0.0123 | 4.18 |
| `aalto_grp_prior_mean` | 2.99x | 0.0075 | 3.15 |
| `s2_mi_lognormal` | 2.95x | 0.0125 | 4.61 |
| `s2_cumulative_cloglog` | 2.94x | 0.0125 | 3.69 |
| `sw_re_gauss` | 2.94x | 0.0121 | 4.57 |
| `log10earn_height` | 2.93x | 0.0148 | 2.86 |
| `bym2_offset_only` | 2.91x | 0.0855 | 4.42 |
| `logearn_height` | 2.89x | 0.0149 | 2.86 |
| `earn_height` | 2.87x | 0.0146 | 2.85 |
| `ch15_m15_6` | 2.86x | 0.00816 | 2.98 |
| `Mtbh_model` | 2.85x | 0.0561 | 5.08 |
| `dogs_nonhierarchical` | 2.85x | 0.0587 | 7.02 |
| `sesame_one_pred_a` | 2.84x | 0.00935 | 2.82 |
| `pilots` | 2.83x | 0.00987 | 3.41 |
| `GLMM_Poisson_model` | 2.82x | 0.00938 | 3.83 |
| `sw_student` | 2.79x | 0.0101 | 3.87 |
| `s2_mmc` | 2.77x | 0.0169 | 6.95 |
| `ch13_m13_4nc` | 2.77x | 0.0288 | 3.36 |
| `kidscore_momiq` | 2.76x | 0.0102 | 2.88 |
| `sw_hurdle_lognormal` | 2.76x | 0.0153 | 3.58 |
| `cm_geg` | 2.75x | 0.0654 | 4 |
| `aalto_lin_std` | 2.75x | 0.00833 | 3.32 |
| `s2_hurdle_cumulative` | 2.73x | 0.0177 | 4.18 |
| `blr` | 2.72x | 0.00755 | 3.15 |
| `s2_gr_student` | 2.7x | 0.0139 | 5.19 |
| `cm_exgaussian` | 2.7x | 0.0422 | 3.72 |
| `hierarchical_gp` | 2.68x | 0.0622 | 9.16 |
| `aalto_lin` | 2.67x | 0.00779 | 3.1 |
| `eight_schools_centered` | 2.66x | 0.00713 | 3 |
| `aalto_grp_prior_mean_var` | 2.65x | 0.00834 | 3.93 |
| `ch12_m12_7` | 2.64x | 2.05 | 8.78 |
| `GLM_Poisson_model` | 2.64x | 0.00814 | 3.32 |
| `sw_gamma` | 2.63x | 0.00954 | 3.75 |
| `kidscore_momhs` | 2.63x | 0.00967 | 2.89 |
| `accel_splines` | 2.6x | 0.0203 | 4.42 |
| `ch12_m12_5` | 2.58x | 2.14 | 8.88 |
| `sw_hurdle_pois` | 2.57x | 0.0152 | 3.42 |
| `accel_gp` | 2.55x | 0.0271 | 5.49 |
| `s2_me2` | 2.53x | 0.0158 | 6.71 |
| `ldaK5` | 2.53x | 5.11 | 15.6 |
| `s2_gev` | 2.51x | 0.0148 | 3.79 |
| `s2_sar` | 2.48x | 0.0182 | 4.09 |
| `s2_sar_error` | 2.47x | 0.0183 | 4.15 |
| `sw_acat_cs` | 2.46x | 0.157 | 5.35 |
| `sw_skewnormal` | 2.46x | 0.0116 | 3.96 |
| `losscurve_sislob` | 2.43x | 0.0152 | 4.32 |
| `ch14_m14_3` | 2.42x | 0.0344 | 5.94 |
| `i319_pois_re2` | 2.4x | 0.0248 | 4.86 |
| `s2_rate` | 2.4x | 0.0106 | 3.65 |
| `ch15_m15_3` | 2.4x | 0.035 | 2.72 |
| `logistic_regression_rhs` | 2.39x | 0.109 | 5.14 |
| `Mh_model` | 2.38x | 0.0474 | 3.4 |
| `ch15_m15_4` | 2.38x | 0.0314 | 2.69 |
| `seeds_centered_model` | 2.36x | 0.0101 | 3.86 |
| `s2_mv_shared_re` | 2.35x | 0.0211 | 7.09 |
| `ch11_m11_10` | 2.29x | 0.00798 | 3.24 |
| `sw_re_bern` | 2.28x | 0.0126 | 4.64 |
| `hier_2pl` | 2.27x | 0.39 | 7.7 |
| `sw_vonmises` | 2.25x | 0.00997 | 3.71 |
| `i320_mi_nhanes` | 2.24x | 0.0153 | 4.42 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.23x | 0.102 | 3.33 |
| `extra_hurdle_poisson` | 2.23x | 0.00716 | 2.63 |
| `dogs_hierarchical` | 2.2x | 0.0543 | 2.86 |
| `ch13_m13_4` | 2.2x | 0.0276 | 3.45 |
| `ch09_m9_2` | 2.2x | 0.00671 | 2.55 |
| `ldaK2` | 2.18x | 0.119 | 3.97 |
| `ch13_m13_4b` | 2.18x | 0.0237 | 3.34 |
| `ch09_m9_3` | 2.17x | 0.00645 | 2.51 |
| `lsat_model` | 2.17x | 0.0966 | 3.95 |
| `sw_zi_poisson` | 2.15x | 0.0144 | 3.41 |
| `ch09_m9_5` | 2.15x | 0.00653 | 2.59 |
| `2pl_latent_reg_irt` | 2.15x | 0.149 | 6.13 |
| `sw_re_slope` | 2.15x | 0.0151 | 6.78 |
| `dugongs_model` | 2.14x | 0.00871 | 3.02 |
| `normal_mixture` | 2.14x | 0.096 | 2.8 |
| `ch09_m9_4` | 2.11x | 0.00637 | 2.57 |
| `ch11_m11_5` | 2.11x | 0.0269 | 3.14 |
| `ch13_m13_6` | 2.11x | 0.0278 | 3.44 |
| `aalto_lin_std_t` | 2.11x | 0.0092 | 3.5 |
| `sw_re_pois` | 2.1x | 0.0123 | 4.51 |
| `irt_2pl` | 2.07x | 0.0429 | 4.21 |
| `multi_occupancy` | 2.06x | 0.0845 | 6.17 |
| `low_dim_gauss_mix` | 2.05x | 0.111 | 3.41 |
| `prophet` | 2.04x | 0.0928 | 5.07 |
| `s2_index_mi` | 2.01x | 0.0144 | 4.22 |
| `ch11_m11_4` | 1.98x | 0.0253 | 3.06 |
| `ch11_m11_6` | 1.98x | 0.00796 | 3.03 |
| `seeds_model` | 1.98x | 0.00971 | 3.68 |
| `ch13_m13_7` | 1.98x | 0.00646 | 2.32 |
| `soil_incubation` | 1.97x | 0.0741 | 3.59 |
| `seeds_stanified_model` | 1.96x | 0.00966 | 3.59 |
| `normal_mixture_k` | 1.96x | 0.444 | 4.22 |
| `sw_exgaussian` | 1.95x | 0.0108 | 3.88 |
| `ch11_m11_8` | 1.92x | 0.00771 | 2.94 |
| `ch13_m13_5` | 1.89x | 0.0256 | 3.29 |
| `s2_cens_interval` | 1.89x | 0.014 | 3.8 |
| `ch13_m13_1` | 1.88x | 0.00911 | 3 |
| `ch13_m13_7nc` | 1.88x | 0.00611 | 2.35 |
| `ch09_mp` | 1.86x | 0.00679 | 2.31 |
| `i319_pois_re` | 1.83x | 0.0183 | 4.52 |
| `ch13_m13_3` | 1.83x | 0.00891 | 3.03 |
| `sw_ar` | 1.81x | 0.0138 | 4.26 |
| `sw_arma` | 1.8x | 0.0163 | 4.36 |
| `sw_zi_binomial` | 1.79x | 0.0171 | 3.44 |
| `ch13_m13_2` | 1.79x | 0.00958 | 3.14 |
| `sw_mi` | 1.79x | 0.0136 | 3.87 |
| `lotka_volterra` | 1.78x | 0.0567 | 4.44 |
| `sw_hurdle_gamma` | 1.76x | 0.0177 | 3.57 |
| `cm_lognormal` | 1.75x | 0.0725 | 4.29 |
| `s2_threading` | 1.75x | 0.0104 | 3.38 |
| `cm_choco` | 1.74x | 0.111 | 4.65 |
| `sw_ma` | 1.73x | 0.0142 | 4.25 |
| `sw_asymlaplace` | 1.71x | 0.0151 | 3.56 |
| `wells_dist` | 1.7x | 0.0565 | 3.05 |
| `sw_beta` | 1.69x | 0.0127 | 3.92 |
| `cm_invweibull` | 1.69x | 0.0383 | 4.09 |
| `ch11_m11_11` | 1.68x | 0.00867 | 3.57 |
| `cm_bisa` | 1.68x | 0.0358 | 4.05 |
| `cm_logweibull` | 1.67x | 0.0355 | 4.04 |
| `cm_loggamma` | 1.67x | 0.0442 | 4.22 |
| `s2_mv_subset` | 1.66x | 0.0128 | 3.51 |
| `cm_weibull` | 1.65x | 0.0381 | 4.06 |
| `sw_gaussian` | 1.64x | 0.00909 | 3.32 |
| `GLM_Binomial_model` | 1.64x | 0.00911 | 3.29 |
| `cm_logstudent` | 1.63x | 0.0404 | 4.2 |
| `cm_invgamma` | 1.62x | 0.0399 | 4.07 |
| `cm_gamma` | 1.62x | 0.0377 | 4.06 |
| `sw_zi_negbin` | 1.62x | 0.0182 | 3.49 |
| `surgical_model` | 1.59x | 0.00861 | 3.45 |
| `cm_lnr_bench` | 1.59x | 0.553 | 5.26 |
| `s2_zoi_beta` | 1.57x | 0.0224 | 3.68 |
| `s2_zi_beta` | 1.57x | 0.0217 | 3.67 |
| `cm_betagate` | 1.57x | 0.0833 | 4.1 |
| `ch11_m11_7` | 1.57x | 0.00679 | 2.85 |
| `sw_re_negbin` | 1.54x | 0.0148 | 4.75 |
| `s2_unstr` | 1.52x | 0.0368 | 7.48 |
| `gp_pois_regr` | 1.52x | 0.012 | 5.78 |
| `cm_lba1` | 1.51x | 0.0627 | 4.4 |
| `sw_mv_norescor` | 1.51x | 0.0124 | 3.48 |
| `covid19imperial_v2` | 1.51x | 1.31 | 7.65 |
| `s2_categorical_re` | 1.51x | 0.0269 | 5.44 |
| `gp_regr` | 1.49x | 0.0121 | 5.48 |
| `s2_frechet` | 1.49x | 0.0132 | 3.98 |
| `sw_weibull` | 1.48x | 0.0123 | 3.91 |
| `Rate_4_model` | 1.47x | 0.00685 | 2.53 |
| `Mb_model` | 1.47x | 0.112 | 3.38 |
| `s2_gp_by_gr` | 1.46x | 0.0421 | 9.15 |
| `s2_mixture_theta` | 1.46x | 0.0278 | 4.59 |
| `covid19imperial_v3` | 1.45x | 1.29 | 7.62 |
| `aalto_binom` | 1.45x | 0.00587 | 2.39 |
| `s2_zi_asymlaplace` | 1.45x | 0.0184 | 3.6 |
| `hmm_example` | 1.43x | 0.0569 | 4.14 |
| `ch12_m12_6` | 1.43x | 4.03 | 9.84 |
| `aalto_bern` | 1.42x | 0.00637 | 2.57 |
| `Rate_1_model` | 1.42x | 0.00615 | 2.39 |
| `ch15_m15_7` | 1.4x | 0.0284 | 7.15 |
| `i320_gp_expquad` | 1.39x | 0.0328 | 7.06 |
| `sw_binomial` | 1.39x | 0.012 | 3.58 |
| `cm_exwald` | 1.39x | 0.109 | 4.32 |
| `ch11_m11_9` | 1.37x | 0.00716 | 2.61 |
| `Rate_5_model` | 1.36x | 0.00687 | 2.51 |
| `cm_lba2` | 1.36x | 0.0917 | 4.73 |
| `aalto_binom2` | 1.36x | 0.00636 | 2.5 |
| `ch14_m14_2` | 1.36x | 0.0465 | 7.12 |
| `aalto_binomb` | 1.34x | 0.00623 | 2.42 |
| `sw_mono` | 1.34x | 0.0157 | 3.99 |
| `i319_negbin_re` | 1.34x | 0.03 | 4.79 |
| `ch12_m12_2` | 1.33x | 0.00957 | 3.98 |
| `ch12_m12_1` | 1.32x | 0.00944 | 3.2 |
| `ch14_m14_8nc` | 1.31x | 0.0165 | 5.71 |
| `Rate_3_model` | 1.3x | 0.00696 | 2.39 |
| `Rate_2_model` | 1.3x | 0.00718 | 2.55 |
| `ch14_m14_1` | 1.29x | 0.0197 | 6.89 |
| `ch11_m_pois` | 1.28x | 0.00642 | 2.54 |
| `ch14_m14_6x` | 1.28x | 0.185 | 6.85 |
| `s2_mo_simo_prior` | 1.27x | 0.0155 | 3.99 |
| `sw_mixture` | 1.27x | 0.0239 | 4.82 |
| `sw_gp` | 1.27x | 0.05 | 7.1 |
| `nn_rbm1bJ10` | 1.26x | 0.333 | 5.9 |
| `s2_beta_binomial` | 1.25x | 0.0178 | 3.72 |
| `ch14_m14_6` | 1.25x | 0.183 | 6.84 |
| `cm_rdm` | 1.24x | 0.246 | 4.56 |
| `ch14_m14_9` | 1.24x | 0.689 | 4.92 |
| `sw_mv_rescor` | 1.24x | 0.025 | 6.19 |
| `sw_cratio` | 1.24x | 0.142 | 3.8 |
| `sw_sratio` | 1.23x | 0.138 | 3.76 |
| `ch14_m14_10` | 1.23x | 0.748 | 5 |
| `i320_sratio_plain` | 1.22x | 0.139 | 3.92 |
| `sw_bernoulli` | 1.21x | 0.00872 | 3.26 |
| `s2_custom_vint` | 1.19x | 0.0198 | 3.55 |
| `ch14_m14_7` | 1.18x | 0.0727 | 8.9 |
| `ch14_m14_8` | 1.17x | 0.0156 | 4.5 |
| `s2_cumulative_probit` | 1.17x | 0.0173 | 3.72 |
| `sw_categorical` | 1.17x | 0.0118 | 4.34 |
| `cm_lnr` | 1.15x | 0.309 | 4.66 |
| `sw_cratio_cs` | 1.15x | 0.205 | 4.96 |
| `sw_poisson` | 1.14x | 0.00946 | 3.14 |
| `Survey_model` | 1.13x | 0.127 | 3.1 |
| `s2_multinomial` | 1.12x | 0.0294 | 4.02 |
| `i320_sratio_cs` | 1.12x | 0.207 | 5.23 |
| `ch14_m14_11` | 1.11x | 1.33 | 5.42 |
| `s2_discrete_weibull` | 1.11x | 0.0163 | 3.49 |
| `s2_hurdle_negbin` | 1.1x | 0.0263 | 3.49 |
| `sw_negbinomial` | 1.08x | 0.0116 | 3.37 |
| `nn_rbm1bJ100` | 1.06x | 864 | 924 |
| `s2_weights_trunc` | 1.06x | 0.0235 | 3.63 |
| `wells_dist100_model` | 1.05x | 0.0419 | 3.22 |
| `s2_dirichlet` | 1.04x | 0.0355 | 4.39 |
| `aalto_poisson_simple` | 1.04x | 0.00957 | 2.65 |
| `s2_wiener` | 1.04x | 0.0703 | 3.75 |
| `i319_negbin_fixed` | 1.03x | 0.0246 | 3.42 |
| `wells_dist100ars_model` | 1.03x | 0.0466 | 3.26 |
| `garch11` | 1.02x | 0.0259 | 2.99 |
| `hmm_drive_1` | 1.02x | 0.354 | 5.12 |
| `wells_dae_c_model` | 1.02x | 0.0465 | 3.39 |
| `sw_trunc` | 1.02x | 0.022 | 3.58 |
| `iohmm_reg` | 1.02x | 0.89 | 6.71 |
| `sw_cumulative` | 1.02x | 0.0515 | 3.83 |
| `wells_dae_model` | 1.02x | 0.05 | 3.26 |
| `hmm_gaussian` | 1.02x | 0.69 | 5.36 |
| `wells_dae_inter_model` | 1.02x | 0.0508 | 3.37 |
| `arma11` | 1.01x | 0.0196 | 2.99 |
| `diamonds` | 1.01x | 0.0898 | 3.51 |
| `i320_pois_trunc_both` | 1.01x | 0.0756 | 3.64 |
| `s2_ar_cov` | 1.01x | 0.0331 | 7.2 |
| `nes_logit_model` | 1.01x | 0.0202 | 3.16 |
| `i320_pois_trunc_ub` | 1.01x | 0.0609 | 3.54 |
| `bones_model` | 0.998x | 0.126 | 3.64 |
| `sw_cumulative_cs` | 0.998x | 0.182 | 4.91 |
| `wells_interaction_model` | 0.997x | 0.0495 | 3.28 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.995x | 0.0495 | 3.38 |
| `cm_ddm` | 0.992x | 38.7 | 44.4 |
| `cm_betadiscrete` | 0.987x | 3.96 | 8.09 |
| `i319_pois_fixed` | 0.986x | 0.0144 | 3.13 |
| `wells_daae_c_model` | 0.982x | 0.0507 | 3.42 |
| `one_comp_mm_elim_abs` | 0.982x | 0.996 | 4.42 |
| `s2_cosy` | 0.964x | 0.0305 | 6.48 |
| `hmm_drive_0` | 0.951x | 0.343 | 5.08 |
| `i320_gp_matern32` | 0.932x | 0.073 | 6.98 |
| `s2_fcor` | 0.93x | 0.0375 | 4.93 |
| `aalto_gpareto` | 0.836x | 0.0141 | 2.89 |
| `cm_invgaussian` | 0.763x | 0.262 | 4.76 |
| `s2_com_poisson` | 0.253x | 0.503 | 3.76 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2414) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 4623) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 5236) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->

### CmdStan with stanc3 loop vectorization

Stock pinned stanc3 leaves loop vectorization off at `--O1`. This table turns
that one setting on and runs the resulting compiler with `--O1`, so CmdStan
gets both the O1 optimizations and loop vectorization (this is neither a
vectorization-only ablation nor `--Oexperimental`). The Stanli timings are the
default run's. The CmdStan timings are reused from earlier runs of the same
CmdStan build and toolchain with the same inputs and settings; models those
runs lack are timed again. Every Stanli gradient passes the same numerical gate
against the reused CmdStan gradients. Because the two sides come from separate
runs, the ratio is CmdStan's median gradient time divided by Stanli's.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records the pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
and stock and patched MIR for a scalar normal-likelihood loop are kept with the
[evidence](../output/corpus-performance-vectorized/README.md).

<!--gen:benchmark_vectorized_catalog-->
Stanli timings are from the default run `25ec9a23201be97b`. The CmdStan timings with loop vectorization are reused from run `8ab8978d93458c88` (2026-10-10). `ctsem_ctsm_400` is from run `ad937274cf01193a` (2026-10-16). Each ratio is CmdStan's median gradient time divided by Stanli's.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 31.8x | 0.157 | 7.54 |
| `gpcm_latent_reg_irt` | 12.6x | 0.337 | 8.43 |
| `grsm_latent_reg_irt` | 12.1x | 0.177 | 7 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.5x | 0.0917 | 3.63 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.8x | 0.0951 | 3.8 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.29x | 0.143 | 3.81 |
| `radon_county_intercept` | 8.24x | 0.142 | 3.72 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.19x | 0.148 | 3.82 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 7.83x | 0.145 | 4.01 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.74x | 0.145 | 3.97 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.64x | 0.16 | 3.94 |
| `aalto_poisson_hurdle` | 7.51x | 0.0411 | 3.07 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.41x | 0.215 | 4.22 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.37x | 0.167 | 4.24 |
| `s2_custom_vreal` | 7.18x | 0.00958 | 3.44 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.1x | 0.217 | 4.39 |
| `ch12_m12_3_alt` | 6.79x | 0.0139 | 2.55 |
| `ch12_m12_3` | 6.67x | 0.014 | 2.52 |
| `ch16_m16_1` | 6.62x | 0.0206 | 3.04 |
| `rats_model` | 6.51x | 0.0107 | 3.09 |
| `M0_model` | 6.4x | 0.0149 | 2.74 |
| `ctsem_ctsm_400` | 5.61x | 8.43 | 501 |
| `ctsem_ctsm` | 5.38x | 1.47 | 441 |
| `arK` | 4.91x | 0.0125 | 2.88 |
| `Mt_model` | 4.84x | 0.017 | 3.5 |
| `Mth_model` | 4.07x | 0.0691 | 4.49 |
| `s2_car_icar` | 4.03x | 0.0119 | 4.38 |
| `s2_cumulative_cauchit` | 4.01x | 0.0155 | 3.75 |
| `s2_logistic_normal` | 3.58x | 0.0275 | 6.73 |
| `s2_mm_weights` | 3.51x | 0.0134 | 4.7 |
| `dogs` | 3.49x | 0.0421 | 4.23 |
| `sw_weights` | 3.39x | 0.0102 | 3.46 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.34x | 0.0232 | 4.86 |
| `GLMM1_model` | 3.34x | 0.0317 | 3.27 |
| `s2_car` | 3.32x | 0.0138 | 4.7 |
| `cm_exgaussian` | 3.31x | 0.0422 | 4.42 |
| `sw_me` | 3.3x | 0.0112 | 4.27 |
| `s2_cox_cens` | 3.29x | 0.0144 | 4.98 |
| `s2_mm` | 3.26x | 0.0126 | 4.71 |
| `s2_car_esicar` | 3.22x | 0.0141 | 4.79 |
| `i319_gauss_re` | 3.21x | 0.0146 | 4.58 |
| `ch09_m9_1_chains4` | 3.19x | 0.00825 | 3.32 |
| `s2_me2_nomecor` | 3.15x | 0.0143 | 4.53 |
| `s2_gp_by_approx` | 3.14x | 0.0196 | 6.5 |
| `logmesquite_logvash` | 3.14x | 0.00915 | 3.54 |
| `s2_gr_by` | 3.08x | 0.0123 | 4.93 |
| `sw_re_gauss` | 3.07x | 0.0121 | 4.59 |
| `mesquite` | 3.07x | 0.00949 | 3.38 |
| `s2_dist_sigma_re` | 3.05x | 0.0123 | 4.96 |
| `logmesquite_logva` | 3.03x | 0.0086 | 3.33 |
| `ch15_m15_1` | 3.03x | 0.00938 | 3.41 |
| `ch09_m9_1` | 3.02x | 0.00916 | 3.29 |
| `logmesquite` | 2.99x | 0.0099 | 3.42 |
| `aalto_grp_aov` | 2.98x | 0.00751 | 3.05 |
| `radon_county` | 2.97x | 0.0643 | 3.29 |
| `logmesquite_logvolume` | 2.95x | 0.00781 | 3.03 |
| `logmesquite_logvas` | 2.92x | 0.00942 | 3.65 |
| `s2_cox` | 2.91x | 0.0123 | 4.11 |
| `cm_geg` | 2.83x | 0.0654 | 4.66 |
| `s2_hurdle_cumulative` | 2.82x | 0.0177 | 4.17 |
| `eight_schools_centered` | 2.8x | 0.00713 | 2.99 |
| `aalto_grp_prior_mean` | 2.77x | 0.0075 | 3.13 |
| `s2_gr_student` | 2.77x | 0.0139 | 5.29 |
| `sw_acat` | 2.75x | 0.111 | 4.21 |
| `Mtbh_model` | 2.72x | 0.0561 | 5 |
| `sw_acat_cs` | 2.71x | 0.157 | 5.43 |
| `ch15_m15_5` | 2.7x | 0.00986 | 3.69 |
| `dogs_nonhierarchical` | 2.7x | 0.0587 | 6.9 |
| `sw_hurdle_lognormal` | 2.67x | 0.0153 | 3.55 |
| `ch09_m5_8s` | 2.66x | 0.00864 | 3.03 |
| `hierarchical_gp` | 2.63x | 0.0622 | 9.16 |
| `ch15_m15_9` | 2.61x | 0.0142 | 3.15 |
| `s2_mmc` | 2.59x | 0.0169 | 7.02 |
| `ch15_m15_2` | 2.58x | 0.01 | 3.64 |
| `sw_hurdle_pois` | 2.58x | 0.0152 | 3.42 |
| `s2_s_cc` | 2.55x | 0.00978 | 4.21 |
| `ch15_m15_8` | 2.55x | 0.0132 | 2.9 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.52x | 0.00841 | 3.21 |
| `ch09_m5_8s2` | 2.5x | 0.00828 | 3.03 |
| `ldaK5` | 2.48x | 5.11 | 15.3 |
| `nes` | 2.48x | 0.0484 | 3.7 |
| `kilpisjarvi` | 2.47x | 0.00786 | 2.8 |
| `ch12_m12_7` | 2.46x | 2.05 | 8.35 |
| `s2_gp_approx` | 2.44x | 0.0136 | 5.51 |
| `aalto_grp_prior_mean_var` | 2.44x | 0.00834 | 3.86 |
| `s2_shifted_lognormal` | 2.43x | 0.0105 | 3.93 |
| `aalto_lin_std` | 2.43x | 0.00833 | 3.23 |
| `pilots` | 2.43x | 0.00987 | 3.49 |
| `ch14_m14_3` | 2.39x | 0.0344 | 5.92 |
| `sw_spline_s` | 2.39x | 0.0112 | 4.35 |
| `s2_nlf` | 2.37x | 0.0107 | 4.35 |
| `sw_skewnormal` | 2.36x | 0.0116 | 3.92 |
| `ch12_m12_5` | 2.36x | 2.14 | 8.37 |
| `i320_mi_nhanes` | 2.36x | 0.0153 | 4.43 |
| `kidscore_interaction_z` | 2.33x | 0.0131 | 3.27 |
| `aalto_lin` | 2.33x | 0.00779 | 3.05 |
| `kidscore_interaction_c2` | 2.33x | 0.0125 | 3.15 |
| `sw_re_bern` | 2.33x | 0.0126 | 4.66 |
| `i319_pois_re2` | 2.32x | 0.0248 | 4.82 |
| `s2_cumulative_cloglog` | 2.31x | 0.0125 | 3.77 |
| `accel_gp` | 2.31x | 0.0271 | 6.62 |
| `sw_vonmises` | 2.29x | 0.00997 | 3.65 |
| `s2_me2` | 2.29x | 0.0158 | 6.68 |
| `Mh_model` | 2.28x | 0.0474 | 3.36 |
| `s2_mv_shared_re` | 2.28x | 0.0211 | 7.17 |
| `s2_sar_error` | 2.27x | 0.0183 | 4.33 |
| `ch13_m13_4b` | 2.26x | 0.0237 | 3.34 |
| `kidscore_interaction` | 2.24x | 0.0131 | 3.21 |
| `s2_sar` | 2.24x | 0.0182 | 4.24 |
| `kidscore_mom_work` | 2.23x | 0.014 | 3.12 |
| `kidscore_interaction_c` | 2.23x | 0.0134 | 3.18 |
| `logearn_height_male` | 2.23x | 0.0183 | 3.06 |
| `normal_mixture` | 2.22x | 0.096 | 3.01 |
| `logearn_logheight_male` | 2.22x | 0.0185 | 3.09 |
| `losscurve_sislob` | 2.22x | 0.0152 | 4.71 |
| `ch13_m13_6` | 2.22x | 0.0278 | 3.41 |
| `sw_nonlinear` | 2.22x | 0.0105 | 3.89 |
| `s2_gev` | 2.21x | 0.0148 | 3.84 |
| `ch11_m11_5` | 2.2x | 0.0269 | 3.13 |
| `logearn_interaction_z` | 2.2x | 0.0234 | 3.32 |
| `ch09_m9_5` | 2.2x | 0.00653 | 2.56 |
| `ch13_m13_4` | 2.2x | 0.0276 | 3.43 |
| `dogs_hierarchical` | 2.19x | 0.0543 | 2.86 |
| `sw_re_slope` | 2.19x | 0.0151 | 6.83 |
| `sw_lognormal` | 2.19x | 0.00937 | 3.63 |
| `ch15_m15_6` | 2.18x | 0.00816 | 3.19 |
| `kidscore_momhsiq` | 2.18x | 0.0112 | 3.1 |
| `ch09_m9_3` | 2.18x | 0.00645 | 2.52 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.18x | 0.102 | 3.32 |
| `extra_hurdle_poisson` | 2.17x | 0.00716 | 2.59 |
| `ch13_m13_4nc` | 2.16x | 0.0288 | 3.37 |
| `ch14_m14_5` | 2.16x | 0.0119 | 3.04 |
| `dugongs_model` | 2.15x | 0.00871 | 3.03 |
| `logearn_interaction` | 2.15x | 0.024 | 3.18 |
| `s2_mi_lognormal` | 2.15x | 0.0125 | 4.56 |
| `ch09_m9_4` | 2.13x | 0.00637 | 2.58 |
| `GLMM_Poisson_model` | 2.12x | 0.00938 | 3.85 |
| `low_dim_gauss_mix` | 2.1x | 0.111 | 3.43 |
| `accel_splines` | 2.1x | 0.0203 | 4.59 |
| `s2_s_by` | 2.09x | 0.0229 | 5.74 |
| `ch09_m9_2` | 2.07x | 0.00671 | 2.54 |
| `ch14_m14_4x` | 2.06x | 0.0107 | 2.87 |
| `seeds_centered_model` | 2.05x | 0.0101 | 3.91 |
| `sw_re_pois` | 2.05x | 0.0123 | 4.51 |
| `ldaK2` | 2.04x | 0.119 | 3.88 |
| `sw_se` | 2.03x | 0.00899 | 3.44 |
| `sw_dist_sigma` | 2.03x | 0.0126 | 3.79 |
| `logearn_height` | 2.03x | 0.0149 | 2.9 |
| `log10earn_height` | 2.02x | 0.0148 | 2.89 |
| `sesame_one_pred_a` | 2.02x | 0.00935 | 2.83 |
| `ch11_m11_4` | 2.01x | 0.0253 | 3.05 |
| `s2_nl_noloop` | 2.01x | 0.00967 | 3.83 |
| `sw_student` | 2x | 0.0101 | 3.8 |
| `radon_pooled` | 2x | 0.0934 | 3.02 |
| `aalto_lin_std_t` | 2x | 0.0092 | 3.46 |
| `sw_spline_t2` | 2x | 0.0152 | 4.82 |
| `ch11_m11_10` | 1.98x | 0.00798 | 3.23 |
| `earn_height` | 1.98x | 0.0146 | 2.86 |
| `election88_full` | 1.98x | 0.486 | 6.04 |
| `sw_zi_poisson` | 1.98x | 0.0144 | 3.39 |
| `ch11_m11_6` | 1.98x | 0.00796 | 3 |
| `kidscore_momhs` | 1.97x | 0.00967 | 2.92 |
| `ch13_m13_5` | 1.97x | 0.0256 | 3.28 |
| `ch14_m14_4` | 1.97x | 0.0102 | 2.87 |
| `GLM_Poisson_model` | 1.96x | 0.00814 | 3.3 |
| `ch13_m13_1` | 1.95x | 0.00911 | 2.98 |
| `ch16_m16_4` | 1.93x | 0.0108 | 3.35 |
| `kidscore_momiq` | 1.93x | 0.0102 | 2.91 |
| `s2_cens_interval` | 1.91x | 0.014 | 3.74 |
| `multi_occupancy` | 1.89x | 0.0845 | 6.56 |
| `normal_mixture_k` | 1.88x | 0.444 | 4.2 |
| `ch13_m13_3` | 1.87x | 0.00891 | 3.01 |
| `ch11_m11_8` | 1.87x | 0.00771 | 2.92 |
| `ch13_m13_7nc` | 1.86x | 0.00611 | 2.34 |
| `ch13_m13_2` | 1.84x | 0.00958 | 3.12 |
| `s2_t2_by` | 1.83x | 0.0204 | 5.68 |
| `s2_index_mi` | 1.83x | 0.0144 | 4.17 |
| `ch09_mp` | 1.83x | 0.00679 | 2.32 |
| `soil_incubation` | 1.83x | 0.0741 | 4.03 |
| `ch13_m13_7` | 1.81x | 0.00646 | 2.33 |
| `lotka_volterra` | 1.81x | 0.0567 | 4.55 |
| `i319_pois_re` | 1.8x | 0.0183 | 4.52 |
| `sw_hurdle_gamma` | 1.78x | 0.0177 | 3.54 |
| `sw_asymlaplace` | 1.77x | 0.0151 | 3.64 |
| `cm_loggamma` | 1.77x | 0.0442 | 4.65 |
| `ch15_m15_3` | 1.74x | 0.035 | 2.7 |
| `sw_mi` | 1.72x | 0.0136 | 3.83 |
| `hier_2pl` | 1.72x | 0.39 | 7.41 |
| `sw_zi_binomial` | 1.71x | 0.0171 | 3.43 |
| `cm_logweibull` | 1.7x | 0.0355 | 4.09 |
| `ch11_m11_11` | 1.69x | 0.00867 | 3.55 |
| `bym2_offset_only` | 1.69x | 0.0855 | 4.5 |
| `cm_lognormal` | 1.69x | 0.0725 | 5.52 |
| `cm_bisa` | 1.68x | 0.0358 | 4.16 |
| `2pl_latent_reg_irt` | 1.67x | 0.149 | 5.76 |
| `sw_ar` | 1.67x | 0.0138 | 4.25 |
| `seeds_stanified_model` | 1.66x | 0.00966 | 3.65 |
| `ch15_m15_4` | 1.66x | 0.0314 | 2.76 |
| `s2_threading` | 1.64x | 0.0104 | 3.38 |
| `cm_invweibull` | 1.63x | 0.0383 | 4.07 |
| `cm_weibull` | 1.63x | 0.0381 | 4.06 |
| `cm_betagate` | 1.63x | 0.0833 | 4.32 |
| `ch11_m11_7` | 1.61x | 0.00679 | 2.85 |
| `s2_mv_subset` | 1.6x | 0.0128 | 3.49 |
| `cm_choco` | 1.6x | 0.111 | 5.15 |
| `sw_ma` | 1.6x | 0.0142 | 4.23 |
| `sw_arma` | 1.6x | 0.0163 | 4.34 |
| `cm_lba1` | 1.59x | 0.0627 | 4.97 |
| `cm_logstudent` | 1.59x | 0.0404 | 4.22 |
| `sw_zi_negbin` | 1.58x | 0.0182 | 3.48 |
| `cm_invgamma` | 1.58x | 0.0399 | 4.09 |
| `surgical_model` | 1.56x | 0.00861 | 3.43 |
| `seeds_model` | 1.56x | 0.00971 | 3.73 |
| `cm_gamma` | 1.55x | 0.0377 | 4.11 |
| `covid19imperial_v3` | 1.54x | 1.29 | 7.6 |
| `sw_gamma` | 1.52x | 0.00954 | 3.76 |
| `prophet` | 1.51x | 0.0928 | 5.68 |
| `sw_gaussian` | 1.51x | 0.00909 | 3.31 |
| `s2_categorical_re` | 1.51x | 0.0269 | 5.45 |
| `s2_unstr` | 1.51x | 0.0368 | 8.7 |
| `cm_lnr_bench` | 1.5x | 0.553 | 8.96 |
| `Mb_model` | 1.5x | 0.112 | 3.39 |
| `sw_cratio_cs` | 1.5x | 0.205 | 5.3 |
| `sw_re_negbin` | 1.49x | 0.0148 | 4.76 |
| `sw_mv_norescor` | 1.47x | 0.0124 | 3.49 |
| `lsat_model` | 1.47x | 0.0966 | 3.86 |
| `s2_zi_asymlaplace` | 1.47x | 0.0184 | 3.67 |
| `gp_regr` | 1.46x | 0.0121 | 5.46 |
| `aalto_binom` | 1.45x | 0.00587 | 2.37 |
| `gp_pois_regr` | 1.44x | 0.012 | 5.64 |
| `covid19imperial_v2` | 1.44x | 1.31 | 7.57 |
| `aalto_bern` | 1.43x | 0.00637 | 2.56 |
| `Rate_4_model` | 1.43x | 0.00685 | 2.51 |
| `i320_sratio_cs` | 1.42x | 0.207 | 5.58 |
| `s2_rate` | 1.41x | 0.0106 | 3.6 |
| `Rate_1_model` | 1.41x | 0.00615 | 2.37 |
| `ch14_m14_2` | 1.4x | 0.0465 | 7.11 |
| `s2_zoi_beta` | 1.39x | 0.0224 | 3.77 |
| `cm_exwald` | 1.39x | 0.109 | 5.98 |
| `hmm_example` | 1.39x | 0.0569 | 4.14 |
| `ch11_m11_9` | 1.39x | 0.00716 | 2.6 |
| `s2_zi_beta` | 1.39x | 0.0217 | 3.72 |
| `cm_lba2` | 1.38x | 0.0917 | 7.45 |
| `GLM_Binomial_model` | 1.38x | 0.00911 | 3.28 |
| `s2_mixture_theta` | 1.38x | 0.0278 | 4.47 |
| `sw_cumulative_cs` | 1.38x | 0.182 | 5.17 |
| `sw_exgaussian` | 1.38x | 0.0108 | 3.87 |
| `i320_gp_expquad` | 1.38x | 0.0328 | 7.31 |
| `aalto_binom2` | 1.37x | 0.00636 | 2.5 |
| `aalto_binomb` | 1.37x | 0.00623 | 2.42 |
| `s2_cumulative_probit` | 1.36x | 0.0173 | 3.69 |
| `i319_negbin_re` | 1.36x | 0.03 | 4.79 |
| `ch15_m15_7` | 1.35x | 0.0284 | 7.62 |
| `irt_2pl` | 1.34x | 0.0429 | 4.27 |
| `ch12_m12_1` | 1.34x | 0.00944 | 3.19 |
| `Rate_2_model` | 1.34x | 0.00718 | 2.55 |
| `blr` | 1.31x | 0.00755 | 3.06 |
| `ch14_m14_8nc` | 1.31x | 0.0165 | 5.99 |
| `ch11_m_pois` | 1.3x | 0.00642 | 2.52 |
| `ch12_m12_2` | 1.3x | 0.00957 | 3.97 |
| `Rate_5_model` | 1.3x | 0.00687 | 2.49 |
| `Rate_3_model` | 1.29x | 0.00696 | 2.4 |
| `wells_dist` | 1.29x | 0.0565 | 3.07 |
| `sw_gp` | 1.28x | 0.05 | 7.32 |
| `ch12_m12_6` | 1.28x | 4.03 | 9.31 |
| `ch14_m14_6` | 1.28x | 0.183 | 6.96 |
| `ch14_m14_6x` | 1.27x | 0.185 | 6.93 |
| `nn_rbm1bJ10` | 1.27x | 0.333 | 5.85 |
| `ch14_m14_1` | 1.26x | 0.0197 | 6.9 |
| `sw_beta` | 1.26x | 0.0127 | 3.9 |
| `ch14_m14_10` | 1.23x | 0.748 | 5.15 |
| `sw_mixture` | 1.23x | 0.0239 | 4.74 |
| `sw_categorical` | 1.23x | 0.0118 | 4.37 |
| `ch14_m14_9` | 1.23x | 0.689 | 5.07 |
| `sw_mv_rescor` | 1.22x | 0.025 | 6.11 |
| `sw_bernoulli` | 1.22x | 0.00872 | 3.27 |
| `ch14_m14_8` | 1.21x | 0.0156 | 4.77 |
| `sw_weibull` | 1.18x | 0.0123 | 3.92 |
| `s2_frechet` | 1.18x | 0.0132 | 3.95 |
| `s2_custom_vint` | 1.17x | 0.0198 | 3.54 |
| `ch14_m14_7` | 1.17x | 0.0727 | 8.86 |
| `sw_binomial` | 1.17x | 0.012 | 3.49 |
| `cm_rdm` | 1.17x | 0.246 | 10.8 |
| `logistic_regression_rhs` | 1.15x | 0.109 | 5.14 |
| `Survey_model` | 1.13x | 0.127 | 3.09 |
| `sw_poisson` | 1.13x | 0.00946 | 3.11 |
| `cm_lnr` | 1.11x | 0.309 | 14 |
| `sw_negbinomial` | 1.11x | 0.0116 | 3.41 |
| `sw_sratio` | 1.11x | 0.138 | 4.05 |
| `i320_sratio_plain` | 1.1x | 0.139 | 4.18 |
| `s2_multinomial` | 1.1x | 0.0294 | 3.95 |
| `ch14_m14_11` | 1.09x | 1.33 | 5.82 |
| `sw_cratio` | 1.09x | 0.142 | 4.04 |
| `s2_beta_binomial` | 1.08x | 0.0178 | 3.7 |
| `nn_rbm1bJ100` | 1.08x | 864 | 933 |
| `sw_mono` | 1.07x | 0.0157 | 4.13 |
| `s2_hurdle_negbin` | 1.06x | 0.0263 | 3.51 |
| `sw_trunc` | 1.05x | 0.022 | 3.54 |
| `hmm_gaussian` | 1.05x | 0.69 | 5.39 |
| `s2_discrete_weibull` | 1.05x | 0.0163 | 3.51 |
| `s2_weights_trunc` | 1.04x | 0.0235 | 3.59 |
| `wells_dae_inter_model` | 1.04x | 0.0508 | 3.4 |
| `aalto_poisson_simple` | 1.04x | 0.00957 | 2.67 |
| `i319_negbin_fixed` | 1.04x | 0.0246 | 3.41 |
| `wells_dist100_model` | 1.04x | 0.0419 | 3.23 |
| `wells_dae_model` | 1.03x | 0.05 | 3.28 |
| `bones_model` | 1.03x | 0.126 | 3.6 |
| `wells_dae_c_model` | 1.02x | 0.0465 | 3.41 |
| `diamonds` | 1.02x | 0.0898 | 3.49 |
| `hmm_drive_1` | 1.02x | 0.354 | 5.08 |
| `sw_cumulative` | 1.01x | 0.0515 | 3.78 |
| `i319_pois_fixed` | 1.01x | 0.0144 | 3.13 |
| `s2_dirichlet` | 1.01x | 0.0355 | 4.39 |
| `one_comp_mm_elim_abs` | 1.01x | 0.996 | 4.8 |
| `s2_wiener` | 1x | 0.0703 | 3.75 |
| `wells_interaction_model` | 0.995x | 0.0495 | 3.3 |
| `hmm_drive_0` | 0.989x | 0.343 | 5.1 |
| `s2_ar_cov` | 0.988x | 0.0331 | 7.61 |
| `nes_logit_model` | 0.987x | 0.0202 | 3.18 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.987x | 0.0495 | 3.36 |
| `wells_daae_c_model` | 0.985x | 0.0507 | 3.42 |
| `wells_dist100ars_model` | 0.984x | 0.0466 | 3.27 |
| `i320_pois_trunc_both` | 0.983x | 0.0756 | 3.58 |
| `i320_pois_trunc_ub` | 0.979x | 0.0609 | 3.5 |
| `s2_mo_simo_prior` | 0.977x | 0.0155 | 4.1 |
| `iohmm_reg` | 0.973x | 0.89 | 6.82 |
| `cm_betadiscrete` | 0.97x | 3.96 | 8.12 |
| `s2_cosy` | 0.957x | 0.0305 | 6.72 |
| `s2_fcor` | 0.942x | 0.0375 | 4.89 |
| `cm_ddm` | 0.923x | 38.7 | 44.9 |
| `i320_gp_matern32` | 0.905x | 0.073 | 7.23 |
| `cm_invgaussian` | 0.835x | 0.262 | 33.8 |
| `garch11` | 0.833x | 0.0259 | 2.95 |
| `aalto_gpareto` | 0.787x | 0.0141 | 3.06 |
| `arma11` | 0.68x | 0.0196 | 2.98 |
| `s2_com_poisson` | 0.21x | 0.503 | 4.76 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2414) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 4623) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 5236) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->

## Fast mode

These figures are from 2026-10-10 (Stanli 0.20.0, main `84e60796`) and have
not been remeasured. [Fast mode](fast-mode.md) used the same protocol, inputs
and binaries as that day's default run, with models compiled and run in fast
mode (`corpus_bench.py --fast-math`). The default-mode figures here come from
the same 2026-10-10 run. Gradient times, 336 models with paired timings, Apple
M3 Ultra:

- Fast mode is 2.03x faster than default mode in geometric mean; the median
  model is 1.06x faster. 87 models are at least 2x faster, 43 at least 10x
  and 7 at least 100x. Most of the large gains are models whose likelihood
  repeats over the data, which fast mode evaluates once per distinct row or
  group.
- Against CmdStan, the geometric-mean speedup is 1.95x in default mode and
  3.95x in fast mode. Fast mode beats CmdStan on 321 of the 336 models,
  default mode on 307.
- No model is measurably slower in fast mode. The few that timed up to 6%
  slower were within noise when rechecked in CPU cycles.

The largest gains, gradient time in microseconds:

| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 40.1 | 0.081 | 493x faster |
| `nes` | 18.2 | 0.072 | 251x faster |
| `logearn_interaction_z` | 8.15 | 0.051 | 159x faster |
| `diamonds` | 33.6 | 0.311 | 108x faster |
| `ch12_m12_4` | 65.8 | 0.666 | 99x faster |
| `aalto_poisson_hurdle` | 7.34 | 0.208 | 35x faster |
| `kidscore_momiq` | 1.39 | 0.053 | 26x faster |
| `M0_model` | 2.45 | 0.241 | 10x faster |

The per-model fast-mode table, raw observations and build identity are in
[`output/corpus-performance-fast/`](../output/corpus-performance-fast/README.md).

## Reproduction

| Item | Configuration |
| --- | --- |
| Scope | All 343 application models, `--corpus all` |
| Gradient trials | Six alternating pairs, 200 ms warmup and 250 ms measurement per process |
| Displayed workload | Setup + 2,000 × median warm gradient time |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled, lite mode disabled |
| Upstream source base | `2c9d67b98b196e800dc051460aaaf653ac811ace`; exact measured checkpoint and hashes in manifests |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan | 2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6` |
| Stan Math | `5252d51d47c1d5e78005fc043ad996fad6dd8da8` |
| Timeouts | Gradient command 60 s; build 900 s |
| Host | Apple M3 Ultra, macOS arm64, 32 logical CPUs, 96 GiB RAM |

The sweeps run one at a time on a shared development host with normal
background services. Each command asks for one thread from Stan, OpenMP and
common BLAS libraries.

Build the dependencies and `bench_grad`, then run the default sweep and the
loop-vectorization sweep with separate output paths:

```sh
./tools/dev_setup.sh
cmake --build build-rel --target bench_grad -j 4
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/default-v4.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/vectorized-v4.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900 \
  --stanc build/corpus-current/stanc-o1vec-src/_build/default/src/stanc/stanc.exe \
  --stancflags=--O1
```

`--corpus` selects a source collection and `--filter` selects model IDs.
CmdStan binaries are built once and reused from a content-addressed cache,
`$STANLI_BENCH_CACHE` or `~/.cache/stanli/corpus-bench`; `--build-cache DIR`
overrides it and `--no-build-cache` builds inside the run directory.
`--build-jobs N` (default 8) sets how many gradient driver builds run at once.
An entry is keyed by the `.stan` bytes, the stanc binary and flags, the gradient
driver and timer sources, the CmdStan checkout, library, compiler and
environment identity, and the CmdStan path, so worktrees share entries. It holds
the model header, the gradient driver, the ordinary executable and the original
stanc and build durations; Stanli compilation, preparation and every gradient
are always measured fresh. The run directory records the cache location and each
model's hit or miss in `build-prepass.jsonl` and `*.result.json`.
`--stanc` (alias `--cmdstan-stanc`) and `--stancflags` choose CmdStan's
compiler; Stanli uses `deps/stanc3/stanli-vectorize-probe`. `--resume`
requires identical sources, inputs, binaries and settings. Timeouts kill the
command's process group and never count as a completed measurement.

Each `OUT.tsv` has a sibling `OUT.tsv.run/` directory that keeps an immutable
manifest (source, input and executable hashes, machine, threads, compilers,
upstream checkouts and build flags), the frozen inputs, exact commands, load
averages, logs, raw paired observations and numerical deviations. `OUT.tsv` is
derived from these records.

Publish the complete runs, then regenerate the docs and browser catalog:

```sh
python3 tools/publish_corpus_bench.py build/corpus-current/default-v4.tsv \
  output/corpus-performance
V=output/corpus-performance-vectorized
python3 tools/publish_corpus_bench.py build/corpus-current/vectorized-v4.tsv \
  $V/cmdstan-runs/RUN_ID --compiler-evidence build/corpus-current
python3 tools/publish_corpus_bench.py --vectorized $V \
  --stanli-run output/corpus-performance \
  --cmdstan-run $V/cmdstan-runs/RUN_ID [--cmdstan-run ...]
python3 tools/gen_docs.py
python3 tools/gen_web_models.py deps/posteriordb
```

The publisher checks the whole inventory, the raw pairs and the recomputed
estimates, and never fills a missing cell from an older run. The one exception
is the loop-vectorization table: when CmdStan, its toolchain and the patched
stanc3 are unchanged, you can skip the vectorized sweep, list the existing runs
under `cmdstan-runs/`, and time only the models they lack with
`--filter MODEL`. A later `--cmdstan-run` takes precedence for the models it
contains. `publish_corpus_bench.py --vectorized` records which run supplied
each model in `combined.json`, and the renderer rechecks inputs, settings,
toolchain, compiler hash and the numerical gate.

The [default evidence](../output/corpus-performance/README.md) and
[loop-vectorization evidence](../output/corpus-performance-vectorized/README.md)
directories hold the full inventories, manifests, summary TSVs, raw
observations, command events and checksums. Older runs and protocols are in
[benchmark history](../notes/performance/benchmark-history.md).

### Compiler-pipeline regression checks

A change to the compiler pipeline must benchmark both the current default
branch and the candidate before merge. Run the full inventory
(`--corpus all`, no `--filter`) with the shipped configuration for both
revisions, on matched hardware, compiler, build mode, threads, inputs and
settings, with separate output paths.

Compare each model's source-to-MIR time, preparation time, warm gradient and
setup-plus-gradient estimate as `100 * (candidate / baseline - 1)`. A change of
2% or more is a major slowdown. Show per-model and per-phase results, since
an aggregate win can hide a regression. Missing, failed or timed-out
observations stay visible and cannot count as a pass, and every existing
numerical check still applies.

Shared CI hosts can be noisier than 2%. Investigate a threshold crossing with a
separate experiment on an idle host: at least six alternating fresh-process
baseline/candidate pairs for the affected phases, all samples kept, with
identical-binary controls when needed. Do not rerun until a favorable sample
appears. Fix a confirmed regression before merge. The PR lists both revision
SHAs, build identities, exact commands, coverage, the phase comparison and
links to the raw results. The `mir-vectorization-measurements` CI artifact
compares pass-on and pass-off behavior on a small gradient set and does not
replace this check.

### Focused execution-path benchmarks

The corpus run times gradients only. For generated quantities, standalone
function calls and interpreter fallback cliffs, use the public-API phase
benchmarks:

```sh
# From the repository root, after building the runtime and MIR fixtures.
python3 tools/bench_model_phases.py /absolute/path/libstanli.dylib \
  tools/bench_execution_paths.json ode_nested_for
python3 tools/bench_function_paths.py /absolute/path/libstanli.dylib affine
```

The [execution-path manifest](../tools/bench_execution_paths.json) lists the
workloads: nested loops, runtime indexing, loop and branch callbacks, RNG
output and arguments, runtime integer expressions, solver controls in
generated quantities, bounded generated-quantity temporaries, and canaries
that must stay on the interpreter. `bench_function_paths.py` accepts `affine`,
`branch_exit`, `sized`, `nested_identity`, `nested_scalars` and
`nested_matrices`; the nested cases take an optional outer extent (default 4).

`bench_model_phases.py` reports source compilation, preparation, first and
warm gradients, first and warm output rows, and a seeded 100-warmup/100-draw
fit (`--skip-inference` omits the fit). It includes Python/ctypes overhead;
confirm native gradients with `bench_grad --timed`. Peak RSS is whole-process
`getrusage` memory (bytes on macOS, KiB on Linux).

Collect six alternating baseline/candidate pairs in fresh processes with
nothing else running, keep raw samples, hashes and build settings, and check
numerical parity separately. These focused runs do not replace the corpus run
or change its published tables.
