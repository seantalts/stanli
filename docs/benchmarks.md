# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling it reuses a prepared autodiff graph and batches
independent work. Both shorten the first fit and every later gradient, by an
amount that depends on the model.

The current comparison covers **<!--gen:benchmark_models-->343<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->337<!--/gen--> accepted gradient
comparisons, the median model's gradient is **<!--gen:corpus_median-->2.10x<!--/gen--> faster**
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
in-process Python/R first-fit time. CmdStan builds run before any timing
starts, up to `--build-jobs` at a time, so a build duration includes that
contention. A model whose CmdStan inputs match an earlier build reuses that
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
Run `27757819692daf5b` (2026-10-18): 343 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 33x | 0.153 | 7.6 |
| `gpcm_latent_reg_irt` | 11.9x | 0.349 | 8.18 |
| `grsm_latent_reg_irt` | 11.7x | 0.179 | 6.72 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 0.0905 | 3.64 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.5x | 0.0904 | 3.8 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10.1x | 0.144 | 4.2 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 0.142 | 3.99 |
| `radon_county_intercept` | 10x | 0.144 | 3.9 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.78x | 0.148 | 3.99 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.71x | 0.151 | 4.19 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.27x | 0.214 | 4.61 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.25x | 0.218 | 4.5 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.22x | 0.158 | 4.16 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 8.95x | 0.164 | 4.44 |
| `aalto_poisson_hurdle` | 8.38x | 0.0401 | 3.07 |
| `radon_pooled` | 8.32x | 0.0965 | 3.44 |
| `dogs` | 7.95x | 0.0434 | 3.75 |
| `s2_custom_vreal` | 7.67x | 0.0106 | 3.51 |
| `arK` | 7.24x | 0.0128 | 2.91 |
| `rats_model` | 7.22x | 0.0104 | 3.11 |
| `ch12_m12_3_alt` | 6.79x | 0.0141 | 2.57 |
| `ch12_m12_3` | 6.74x | 0.0145 | 2.52 |
| `ch16_m16_1` | 6.63x | 0.0206 | 3.05 |
| `M0_model` | 6.46x | 0.015 | 2.74 |
| `Mt_model` | 5.53x | 0.0174 | 3.49 |
| `sw_nonlinear` | 4.95x | 0.0112 | 3.89 |
| `ch09_m5_8s` | 4.86x | 0.00882 | 2.86 |
| `ch14_m14_5` | 4.62x | 0.0122 | 2.87 |
| `ch09_m5_8s2` | 4.54x | 0.00808 | 2.87 |
| `s2_cumulative_cauchit` | 4.5x | 0.0151 | 3.67 |
| `mesquite` | 4.32x | 0.00961 | 3.1 |
| `logmesquite_logvas` | 4.27x | 0.0101 | 3.41 |
| `logmesquite` | 4.24x | 0.00925 | 3.15 |
| `ch15_m15_1` | 4.2x | 0.00918 | 3.21 |
| `logmesquite_logvash` | 4.13x | 0.00946 | 3.37 |
| `nes` | 4.12x | 0.0468 | 3.38 |
| `logmesquite_logva` | 4.12x | 0.00822 | 3.17 |
| `s2_nl_noloop` | 4.09x | 0.0106 | 3.94 |
| `ch15_m15_2` | 4.05x | 0.0092 | 3.42 |
| `election88_full` | 4x | 0.491 | 6.03 |
| `s2_car_icar` | 3.92x | 0.0117 | 4.42 |
| `sw_spline_s` | 3.91x | 0.012 | 4.49 |
| `ch14_m14_4` | 3.82x | 0.0109 | 2.8 |
| `sw_me` | 3.82x | 0.0123 | 4.16 |
| `ch14_m14_4x` | 3.81x | 0.0104 | 2.82 |
| `s2_s_cc` | 3.76x | 0.0103 | 4.31 |
| `kidscore_interaction_c2` | 3.75x | 0.0132 | 3.01 |
| `Mth_model` | 3.75x | 0.0686 | 4.51 |
| `logmesquite_logvolume` | 3.73x | 0.00832 | 3.02 |
| `kidscore_interaction_c` | 3.65x | 0.0131 | 3.07 |
| `sw_dist_sigma` | 3.62x | 0.0112 | 3.85 |
| `s2_me2_nomecor` | 3.59x | 0.0135 | 4.4 |
| `s2_mm` | 3.52x | 0.0133 | 4.68 |
| `s2_dist_sigma_re` | 3.52x | 0.0138 | 4.91 |
| `logearn_height_male` | 3.51x | 0.0184 | 2.96 |
| `s2_nlf` | 3.5x | 0.0122 | 3.98 |
| `sw_weights` | 3.49x | 0.0105 | 3.51 |
| `kidscore_interaction_z` | 3.49x | 0.0127 | 3.09 |
| `GLMM1_model` | 3.49x | 0.0309 | 3.32 |
| `logearn_interaction_z` | 3.48x | 0.023 | 3.16 |
| `kidscore_mom_work` | 3.47x | 0.0131 | 2.96 |
| `kilpisjarvi` | 3.47x | 0.0075 | 2.8 |
| `kidscore_interaction` | 3.46x | 0.0127 | 3.05 |
| `aalto_grp_aov` | 3.46x | 0.00683 | 3.09 |
| `ch09_m9_1` | 3.46x | 0.0086 | 3.12 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.43x | 0.0231 | 4.9 |
| `sw_se` | 3.42x | 0.00926 | 3.53 |
| `s2_shifted_lognormal` | 3.41x | 0.0103 | 3.95 |
| `ch09_m9_1_chains4` | 3.4x | 0.00917 | 3.1 |
| `s2_cox_cens` | 3.38x | 0.0157 | 5.08 |
| `sw_spline_t2` | 3.37x | 0.0155 | 4.96 |
| `s2_gp_approx` | 3.35x | 0.0146 | 5.23 |
| `s2_car` | 3.35x | 0.0148 | 4.38 |
| `logearn_interaction` | 3.3x | 0.0241 | 3.08 |
| `s2_mm_weights` | 3.3x | 0.013 | 4.69 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.29x | 0.00763 | 3.28 |
| `logearn_logheight_male` | 3.29x | 0.02 | 2.99 |
| `sw_lognormal` | 3.27x | 0.0107 | 3.7 |
| `ch15_m15_5` | 3.26x | 0.00914 | 3.25 |
| `s2_s_by` | 3.25x | 0.0236 | 5.88 |
| `ch16_m16_4` | 3.24x | 0.0103 | 2.93 |
| `ch15_m15_8` | 3.22x | 0.0131 | 2.89 |
| `radon_county` | 3.21x | 0.0649 | 3.29 |
| `s2_car_esicar` | 3.2x | 0.0147 | 4.51 |
| `kidscore_momhsiq` | 3.19x | 0.0111 | 2.96 |
| `s2_gr_by` | 3.17x | 0.0123 | 4.94 |
| `ch15_m15_9` | 3.14x | 0.0145 | 3.04 |
| `sw_acat` | 3.13x | 0.109 | 4.18 |
| `s2_gp_by_approx` | 3.12x | 0.0211 | 6.04 |
| `log10earn_height` | 3.08x | 0.0146 | 2.85 |
| `sw_re_gauss` | 3.05x | 0.012 | 4.56 |
| `s2_t2_by` | 3.05x | 0.0207 | 5.86 |
| `i319_gauss_re` | 2.99x | 0.0156 | 4.58 |
| `s2_cumulative_cloglog` | 2.94x | 0.0132 | 3.69 |
| `GLMM_Poisson_model` | 2.91x | 0.00969 | 3.85 |
| `bym2_offset_only` | 2.9x | 0.0847 | 4.4 |
| `ch15_m15_6` | 2.88x | 0.00851 | 3.01 |
| `s2_gr_student` | 2.88x | 0.0142 | 5.17 |
| `sw_student` | 2.87x | 0.0109 | 3.86 |
| `sesame_one_pred_a` | 2.85x | 0.00933 | 2.83 |
| `s2_mi_lognormal` | 2.85x | 0.0141 | 4.62 |
| `earn_height` | 2.84x | 0.0141 | 2.84 |
| `aalto_grp_prior_mean` | 2.83x | 0.00762 | 3.18 |
| `s2_hurdle_cumulative` | 2.8x | 0.0179 | 4.19 |
| `s2_cox` | 2.8x | 0.0126 | 4.16 |
| `pilots` | 2.77x | 0.00965 | 3.41 |
| `kidscore_momhs` | 2.76x | 0.0102 | 2.88 |
| `sw_hurdle_lognormal` | 2.76x | 0.0146 | 3.58 |
| `Mtbh_model` | 2.75x | 0.0596 | 5.06 |
| `GLM_Poisson_model` | 2.73x | 0.00831 | 3.29 |
| `eight_schools_centered` | 2.73x | 0.00709 | 3 |
| `blr` | 2.73x | 0.00802 | 3.14 |
| `logearn_height` | 2.71x | 0.0149 | 2.87 |
| `dogs_nonhierarchical` | 2.67x | 0.0589 | 7.03 |
| `cm_exgaussian` | 2.65x | 0.0432 | 3.74 |
| `kidscore_momiq` | 2.64x | 0.00987 | 2.88 |
| `ch12_m12_5` | 2.63x | 2.04 | 8.63 |
| `ch12_m12_7` | 2.62x | 2.09 | 8.77 |
| `cm_geg` | 2.61x | 0.0654 | 3.97 |
| `s2_sar_error` | 2.61x | 0.0192 | 4.13 |
| `aalto_lin_std` | 2.59x | 0.00848 | 3.32 |
| `ch13_m13_4nc` | 2.59x | 0.03 | 3.37 |
| `aalto_lin` | 2.57x | 0.00792 | 3.09 |
| `sw_gamma` | 2.56x | 0.0108 | 3.74 |
| `aalto_grp_prior_mean_var` | 2.56x | 0.00814 | 3.91 |
| `accel_gp` | 2.55x | 0.0257 | 5.45 |
| `sw_hurdle_pois` | 2.55x | 0.0164 | 3.37 |
| `s2_mmc` | 2.55x | 0.0178 | 6.99 |
| `accel_splines` | 2.55x | 0.0207 | 4.4 |
| `losscurve_sislob` | 2.54x | 0.0166 | 4.28 |
| `hierarchical_gp` | 2.54x | 0.0625 | 9.15 |
| `ldaK5` | 2.53x | 5.21 | 15.6 |
| `sw_acat_cs` | 2.52x | 0.158 | 5.34 |
| `i319_pois_re2` | 2.47x | 0.0237 | 4.85 |
| `logistic_regression_rhs` | 2.45x | 0.108 | 5.14 |
| `sw_skewnormal` | 2.45x | 0.0125 | 3.97 |
| `s2_gev` | 2.44x | 0.0152 | 3.81 |
| `ch15_m15_4` | 2.41x | 0.0304 | 2.71 |
| `s2_sar` | 2.39x | 0.0187 | 4.11 |
| `ch15_m15_3` | 2.38x | 0.0358 | 2.69 |
| `s2_me2` | 2.37x | 0.0162 | 6.71 |
| `Mh_model` | 2.35x | 0.0478 | 3.37 |
| `sw_re_bern` | 2.33x | 0.0117 | 4.69 |
| `s2_rate` | 2.32x | 0.0101 | 3.64 |
| `ch14_m14_3` | 2.3x | 0.0352 | 5.92 |
| `seeds_centered_model` | 2.3x | 0.00965 | 3.84 |
| `ch11_m11_10` | 2.29x | 0.00779 | 3.26 |
| `lsat_model` | 2.28x | 0.0966 | 3.95 |
| `ch09_m9_5` | 2.25x | 0.00708 | 2.56 |
| `normal_mixture` | 2.24x | 0.0952 | 2.8 |
| `extra_hurdle_poisson` | 2.23x | 0.00736 | 2.63 |
| `ch13_m13_6` | 2.23x | 0.028 | 3.44 |
| `low_dim_gauss_mix` | 2.22x | 0.106 | 3.42 |
| `ldaK2` | 2.22x | 0.117 | 3.93 |
| `ch09_m9_4` | 2.22x | 0.00663 | 2.57 |
| `ch11_m11_5` | 2.2x | 0.0272 | 3.15 |
| `dogs_hierarchical` | 2.19x | 0.0558 | 2.87 |
| `hier_2pl` | 2.19x | 0.401 | 7.7 |
| `sw_vonmises` | 2.17x | 0.011 | 3.7 |
| `i320_mi_nhanes` | 2.17x | 0.0157 | 4.41 |
| `ch13_m13_4` | 2.16x | 0.0279 | 3.47 |
| `dugongs_model` | 2.16x | 0.0104 | 3.03 |
| `aalto_lin_std_t` | 2.16x | 0.0091 | 3.5 |
| `ch13_m13_4b` | 2.15x | 0.0241 | 3.35 |
| `2pl_latent_reg_irt` | 2.14x | 0.144 | 6.21 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.14x | 0.103 | 3.34 |
| `ch09_m9_2` | 2.11x | 0.00683 | 2.53 |
| `s2_mv_shared_re` | 2.11x | 0.0204 | 7.11 |
| `ch09_m9_3` | 2.1x | 0.00685 | 2.54 |
| `sw_zi_poisson` | 2.09x | 0.0154 | 3.39 |
| `s2_index_mi` | 2.08x | 0.0137 | 4.24 |
| `sw_re_pois` | 2.05x | 0.0125 | 4.51 |
| `sw_re_slope` | 2x | 0.016 | 6.79 |
| `soil_incubation` | 2x | 0.0726 | 3.58 |
| `irt_2pl` | 1.99x | 0.0441 | 4.26 |
| `seeds_model` | 1.98x | 0.00997 | 3.69 |
| `prophet` | 1.97x | 0.0943 | 5.09 |
| `ch11_m11_4` | 1.95x | 0.0247 | 3.03 |
| `ch13_m13_5` | 1.95x | 0.0248 | 3.28 |
| `seeds_stanified_model` | 1.94x | 0.0098 | 3.58 |
| `sw_exgaussian` | 1.94x | 0.0114 | 3.91 |
| `multi_occupancy` | 1.93x | 0.0858 | 6.17 |
| `ch13_m13_7` | 1.92x | 0.00627 | 2.3 |
| `normal_mixture_k` | 1.92x | 0.446 | 4.24 |
| `ch13_m13_7nc` | 1.9x | 0.00598 | 2.36 |
| `ch11_m11_6` | 1.9x | 0.00837 | 3.04 |
| `ch09_mp` | 1.86x | 0.00618 | 2.3 |
| `ctsem_ctsm_400` | 1.85x | 22.1 | 128 |
| `ch13_m13_1` | 1.85x | 0.00861 | 2.99 |
| `s2_cens_interval` | 1.84x | 0.0142 | 3.77 |
| `lotka_volterra` | 1.83x | 0.0565 | 4.43 |
| `sw_zi_binomial` | 1.83x | 0.0177 | 3.43 |
| `ch13_m13_3` | 1.82x | 0.00897 | 3.05 |
| `sw_ar` | 1.82x | 0.0147 | 4.27 |
| `sw_arma` | 1.81x | 0.0169 | 4.37 |
| `ch13_m13_2` | 1.81x | 0.00903 | 3.13 |
| `ch11_m11_8` | 1.8x | 0.0079 | 2.96 |
| `sw_mi` | 1.8x | 0.0128 | 3.86 |
| `s2_threading` | 1.8x | 0.00994 | 3.37 |
| `sw_ma` | 1.79x | 0.0151 | 4.27 |
| `cm_bisa` | 1.78x | 0.0366 | 4.06 |
| `ctsem_ctsm` | 1.78x | 2.85 | 92.4 |
| `cm_choco` | 1.74x | 0.109 | 4.68 |
| `sw_hurdle_gamma` | 1.74x | 0.0177 | 3.57 |
| `cm_invweibull` | 1.73x | 0.0395 | 4.06 |
| `cm_loggamma` | 1.72x | 0.0434 | 4.21 |
| `i319_pois_re` | 1.71x | 0.019 | 4.51 |
| `cm_invgamma` | 1.7x | 0.0385 | 4.1 |
| `ch11_m11_11` | 1.7x | 0.00854 | 3.57 |
| `wells_dist` | 1.69x | 0.0571 | 3.04 |
| `cm_logweibull` | 1.69x | 0.0351 | 4.05 |
| `sw_beta` | 1.68x | 0.013 | 3.91 |
| `sw_gaussian` | 1.66x | 0.00977 | 3.29 |
| `cm_gamma` | 1.65x | 0.0378 | 4.08 |
| `GLM_Binomial_model` | 1.65x | 0.00944 | 3.28 |
| `sw_asymlaplace` | 1.64x | 0.0152 | 3.56 |
| `cm_weibull` | 1.62x | 0.0385 | 4.09 |
| `cm_lognormal` | 1.62x | 0.0771 | 4.26 |
| `sw_zi_negbin` | 1.6x | 0.0183 | 3.49 |
| `ch11_m11_7` | 1.6x | 0.00724 | 2.86 |
| `s2_categorical_re` | 1.6x | 0.0261 | 5.44 |
| `s2_mv_subset` | 1.6x | 0.0123 | 3.49 |
| `sw_mv_norescor` | 1.57x | 0.0128 | 3.49 |
| `surgical_model` | 1.57x | 0.00888 | 3.43 |
| `cm_logstudent` | 1.57x | 0.0413 | 4.22 |
| `s2_zoi_beta` | 1.55x | 0.0228 | 3.7 |
| `s2_zi_beta` | 1.55x | 0.0211 | 3.68 |
| `sw_re_negbin` | 1.53x | 0.0156 | 4.77 |
| `cm_betagate` | 1.52x | 0.0859 | 4.08 |
| `cm_lba1` | 1.52x | 0.0603 | 4.43 |
| `s2_unstr` | 1.52x | 0.0385 | 7.5 |
| `s2_frechet` | 1.49x | 0.0128 | 3.98 |
| `s2_zi_asymlaplace` | 1.49x | 0.0178 | 3.62 |
| `s2_gp_by_gr` | 1.49x | 0.0398 | 9.12 |
| `sw_weibull` | 1.47x | 0.0126 | 3.91 |
| `cm_lnr_bench` | 1.47x | 0.57 | 5.25 |
| `gp_pois_regr` | 1.46x | 0.0116 | 5.76 |
| `gp_regr` | 1.46x | 0.0119 | 5.46 |
| `s2_mixture_theta` | 1.46x | 0.0282 | 4.58 |
| `Rate_4_model` | 1.45x | 0.0069 | 2.57 |
| `cm_lba2` | 1.45x | 0.0868 | 4.72 |
| `Rate_1_model` | 1.45x | 0.00582 | 2.41 |
| `covid19imperial_v2` | 1.45x | 1.31 | 7.65 |
| `Mb_model` | 1.44x | 0.114 | 3.4 |
| `covid19imperial_v3` | 1.42x | 1.31 | 7.65 |
| `Rate_2_model` | 1.41x | 0.00716 | 2.55 |
| `aalto_bern` | 1.41x | 0.00686 | 2.57 |
| `i320_gp_expquad` | 1.4x | 0.0329 | 7.05 |
| `ch14_m14_2` | 1.39x | 0.0448 | 7.09 |
| `aalto_binom` | 1.39x | 0.00621 | 2.39 |
| `ch11_m11_9` | 1.39x | 0.00654 | 2.6 |
| `cm_exwald` | 1.38x | 0.107 | 4.33 |
| `hmm_example` | 1.38x | 0.0571 | 4.14 |
| `aalto_binom2` | 1.37x | 0.00671 | 2.49 |
| `ch12_m12_6` | 1.37x | 4.03 | 9.61 |
| `ch11_m_pois` | 1.35x | 0.00676 | 2.54 |
| `ch14_m14_8nc` | 1.34x | 0.0158 | 5.71 |
| `sw_binomial` | 1.33x | 0.0118 | 3.55 |
| `sw_mono` | 1.32x | 0.0151 | 4.01 |
| `ch14_m14_1` | 1.31x | 0.0197 | 6.92 |
| `Rate_5_model` | 1.31x | 0.00696 | 2.5 |
| `ch12_m12_2` | 1.31x | 0.00961 | 3.98 |
| `ch12_m12_1` | 1.3x | 0.0089 | 3.23 |
| `aalto_binomb` | 1.29x | 0.00609 | 2.43 |
| `Rate_3_model` | 1.29x | 0.00644 | 2.4 |
| `ch14_m14_6x` | 1.28x | 0.184 | 6.86 |
| `s2_mo_simo_prior` | 1.28x | 0.0159 | 4.01 |
| `i319_negbin_re` | 1.28x | 0.0313 | 4.77 |
| `sw_gp` | 1.27x | 0.0504 | 7.08 |
| `i320_sratio_plain` | 1.27x | 0.137 | 3.92 |
| `ch14_m14_10` | 1.26x | 0.749 | 5.01 |
| `s2_beta_binomial` | 1.22x | 0.0187 | 3.74 |
| `ch14_m14_6` | 1.22x | 0.186 | 6.9 |
| `nn_rbm1bJ10` | 1.22x | 0.345 | 5.9 |
| `sw_mixture` | 1.22x | 0.0245 | 4.82 |
| `ch14_m14_9` | 1.22x | 0.692 | 4.91 |
| `s2_custom_vint` | 1.22x | 0.0204 | 3.55 |
| `ch14_m14_8` | 1.21x | 0.0156 | 4.49 |
| `sw_sratio` | 1.2x | 0.14 | 3.77 |
| `cm_rdm` | 1.2x | 0.234 | 4.54 |
| `sw_bernoulli` | 1.2x | 0.00955 | 3.26 |
| `sw_cratio` | 1.19x | 0.142 | 3.76 |
| `sw_mv_rescor` | 1.18x | 0.0255 | 6.21 |
| `s2_cumulative_probit` | 1.16x | 0.018 | 3.71 |
| `s2_multinomial` | 1.15x | 0.0296 | 4.02 |
| `ch14_m14_7` | 1.14x | 0.0735 | 8.88 |
| `sw_cratio_cs` | 1.14x | 0.21 | 4.94 |
| `i320_sratio_cs` | 1.13x | 0.209 | 5.25 |
| `Survey_model` | 1.13x | 0.126 | 3.1 |
| `sw_poisson` | 1.13x | 0.00923 | 3.12 |
| `sw_negbinomial` | 1.11x | 0.012 | 3.38 |
| `s2_discrete_weibull` | 1.11x | 0.0172 | 3.47 |
| `cm_lnr` | 1.11x | 0.319 | 4.67 |
| `nn_rbm1bJ100` | 1.08x | 855 | 932 |
| `sw_trunc` | 1.08x | 0.0229 | 3.59 |
| `s2_hurdle_negbin` | 1.07x | 0.0262 | 3.51 |
| `aalto_poisson_simple` | 1.07x | 0.00962 | 2.68 |
| `s2_weights_trunc` | 1.06x | 0.0234 | 3.63 |
| `s2_logistic_normal` | 1.06x | 0.0544 | 6.39 |
| `sw_categorical` | 1.06x | 0.013 | 4.31 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.06x | 0.0482 | 3.37 |
| `wells_dist100_model` | 1.05x | 0.0426 | 3.24 |
| `s2_wiener` | 1.05x | 0.0714 | 3.72 |
| `ch14_m14_11` | 1.05x | 1.42 | 5.42 |
| `ch15_m15_7` | 1.05x | 0.0346 | 7.16 |
| `arma11` | 1.04x | 0.0192 | 3 |
| `s2_dirichlet` | 1.04x | 0.0362 | 4.43 |
| `hmm_gaussian` | 1.04x | 0.704 | 5.38 |
| `wells_interaction_model` | 1.03x | 0.0476 | 3.28 |
| `garch11` | 1.03x | 0.0249 | 2.99 |
| `iohmm_reg` | 1.03x | 0.895 | 6.73 |
| `wells_dist100ars_model` | 1.03x | 0.0462 | 3.28 |
| `nes_logit_model` | 1.02x | 0.0203 | 3.16 |
| `i320_pois_trunc_ub` | 1.01x | 0.0606 | 3.56 |
| `i319_pois_fixed` | 1.01x | 0.0138 | 3.14 |
| `hmm_drive_1` | 1.01x | 0.356 | 5.09 |
| `wells_dae_c_model` | 1.01x | 0.0472 | 3.41 |
| `s2_ar_cov` | 1.01x | 0.0319 | 7.24 |
| `wells_dae_inter_model` | 1.01x | 0.051 | 3.39 |
| `bones_model` | 1.01x | 0.124 | 3.62 |
| `sw_cumulative` | 1.01x | 0.0531 | 3.82 |
| `one_comp_mm_elim_abs` | 1x | 0.979 | 4.41 |
| `sw_cumulative_cs` | 0.995x | 0.185 | 4.88 |
| `wells_dae_model` | 0.992x | 0.0501 | 3.25 |
| `cm_betadiscrete` | 0.991x | 3.99 | 8.14 |
| `wells_daae_c_model` | 0.99x | 0.0513 | 3.44 |
| `s2_cosy` | 0.987x | 0.0301 | 6.51 |
| `cm_ddm` | 0.986x | 38 | 44.3 |
| `diamonds` | 0.983x | 0.0924 | 3.51 |
| `i320_pois_trunc_both` | 0.978x | 0.0766 | 3.63 |
| `i319_negbin_fixed` | 0.967x | 0.0259 | 3.41 |
| `hmm_drive_0` | 0.964x | 0.343 | 5.11 |
| `s2_fcor` | 0.946x | 0.0378 | 4.94 |
| `i320_gp_matern32` | 0.925x | 0.0757 | 6.97 |
| `aalto_gpareto` | 0.842x | 0.0135 | 2.9 |
| `cm_invgaussian` | 0.409x | 0.379 | 4.79 |
| `s2_com_poisson` | 0.258x | 0.496 | 3.81 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2797) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5360) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6075) |
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
Stanli timings are from the default run `27757819692daf5b`. The CmdStan timings with loop vectorization are reused from run `8ab8978d93458c88` (2026-10-10). `s2_gp_by_gr` is from run `532c791f4c877dc4` (2026-10-16). `ctsem_ctsm_400` is from run `ad937274cf01193a` (2026-10-16). Each ratio is CmdStan's median gradient time divided by Stanli's.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 32.7x | 0.153 | 7.54 |
| `grsm_latent_reg_irt` | 11.9x | 0.179 | 7 |
| `gpcm_latent_reg_irt` | 11.8x | 0.349 | 8.43 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.1x | 0.0905 | 3.63 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11x | 0.0904 | 3.8 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.41x | 0.142 | 3.81 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.15x | 0.148 | 3.82 |
| `radon_county_intercept` | 8.13x | 0.144 | 3.72 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8.08x | 0.144 | 4.01 |
| `aalto_poisson_hurdle` | 8.02x | 0.0401 | 3.07 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.64x | 0.151 | 3.97 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.53x | 0.164 | 4.24 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.5x | 0.158 | 3.94 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.44x | 0.214 | 4.39 |
| `s2_custom_vreal` | 7.21x | 0.0106 | 3.44 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.15x | 0.218 | 4.22 |
| `ch12_m12_3_alt` | 7.12x | 0.0141 | 2.55 |
| `ch16_m16_1` | 6.66x | 0.0206 | 3.04 |
| `ch12_m12_3` | 6.55x | 0.0145 | 2.52 |
| `M0_model` | 6.42x | 0.015 | 2.74 |
| `rats_model` | 6.25x | 0.0104 | 3.09 |
| `Mt_model` | 4.94x | 0.0174 | 3.5 |
| `arK` | 4.88x | 0.0128 | 2.88 |
| `Mth_model` | 4.09x | 0.0686 | 4.49 |
| `s2_cumulative_cauchit` | 3.99x | 0.0151 | 3.75 |
| `s2_car_icar` | 3.98x | 0.0117 | 4.38 |
| `dogs` | 3.47x | 0.0434 | 4.23 |
| `GLMM1_model` | 3.43x | 0.0309 | 3.27 |
| `s2_mm_weights` | 3.42x | 0.013 | 4.7 |
| `s2_cox_cens` | 3.42x | 0.0157 | 4.98 |
| `s2_car` | 3.36x | 0.0148 | 4.7 |
| `sw_weights` | 3.35x | 0.0105 | 3.46 |
| `sw_me` | 3.34x | 0.0123 | 4.27 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.28x | 0.0231 | 4.86 |
| `s2_mm` | 3.28x | 0.0133 | 4.71 |
| `cm_exgaussian` | 3.24x | 0.0432 | 4.42 |
| `s2_car_esicar` | 3.15x | 0.0147 | 4.79 |
| `s2_gp_by_approx` | 3.14x | 0.0211 | 6.5 |
| `mesquite` | 3.13x | 0.00961 | 3.38 |
| `s2_me2_nomecor` | 3.11x | 0.0135 | 4.53 |
| `s2_gr_by` | 3.1x | 0.0123 | 4.93 |
| `aalto_grp_aov` | 3.08x | 0.00683 | 3.05 |
| `logmesquite_logvash` | 3.06x | 0.00946 | 3.54 |
| `s2_dist_sigma_re` | 3.05x | 0.0138 | 4.96 |
| `logmesquite` | 3.03x | 0.00925 | 3.42 |
| `i319_gauss_re` | 3.03x | 0.0156 | 4.58 |
| `sw_re_gauss` | 3.02x | 0.012 | 4.59 |
| `ch09_m9_1_chains4` | 2.99x | 0.00917 | 3.32 |
| `logmesquite_logvas` | 2.98x | 0.0101 | 3.65 |
| `radon_county` | 2.97x | 0.0649 | 3.29 |
| `logmesquite_logva` | 2.97x | 0.00822 | 3.33 |
| `ch09_m9_1` | 2.97x | 0.0086 | 3.29 |
| `logmesquite_logvolume` | 2.94x | 0.00832 | 3.03 |
| `ch15_m15_1` | 2.92x | 0.00918 | 3.41 |
| `s2_hurdle_cumulative` | 2.88x | 0.0179 | 4.17 |
| `s2_gr_student` | 2.87x | 0.0142 | 5.29 |
| `ch15_m15_5` | 2.85x | 0.00914 | 3.69 |
| `s2_cox` | 2.82x | 0.0126 | 4.11 |
| `sw_acat` | 2.8x | 0.109 | 4.21 |
| `eight_schools_centered` | 2.79x | 0.00709 | 2.99 |
| `cm_geg` | 2.75x | 0.0654 | 4.66 |
| `sw_hurdle_lognormal` | 2.71x | 0.0146 | 3.55 |
| `aalto_grp_prior_mean` | 2.7x | 0.00762 | 3.13 |
| `sw_acat_cs` | 2.7x | 0.158 | 5.43 |
| `ch09_m5_8s` | 2.69x | 0.00882 | 3.03 |
| `Mtbh_model` | 2.66x | 0.0596 | 5 |
| `dogs_nonhierarchical` | 2.63x | 0.0589 | 6.9 |
| `ch15_m15_8` | 2.63x | 0.0131 | 2.9 |
| `ch15_m15_2` | 2.6x | 0.0092 | 3.64 |
| `nes` | 2.58x | 0.0468 | 3.7 |
| `sw_hurdle_pois` | 2.57x | 0.0164 | 3.42 |
| `ch15_m15_9` | 2.56x | 0.0145 | 3.15 |
| `hierarchical_gp` | 2.55x | 0.0625 | 9.16 |
| `s2_shifted_lognormal` | 2.54x | 0.0103 | 3.93 |
| `s2_s_cc` | 2.53x | 0.0103 | 4.21 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.52x | 0.00763 | 3.21 |
| `ch12_m12_5` | 2.49x | 2.04 | 8.37 |
| `ch09_m5_8s2` | 2.49x | 0.00808 | 3.03 |
| `kilpisjarvi` | 2.48x | 0.0075 | 2.8 |
| `sw_spline_s` | 2.47x | 0.012 | 4.35 |
| `pilots` | 2.47x | 0.00965 | 3.49 |
| `ldaK5` | 2.43x | 5.21 | 15.3 |
| `ch12_m12_7` | 2.42x | 2.09 | 8.35 |
| `s2_gp_approx` | 2.41x | 0.0146 | 5.51 |
| `kidscore_interaction_c2` | 2.4x | 0.0132 | 3.15 |
| `s2_nlf` | 2.38x | 0.0122 | 4.35 |
| `sw_skewnormal` | 2.38x | 0.0125 | 3.92 |
| `s2_mmc` | 2.38x | 0.0178 | 7.02 |
| `aalto_grp_prior_mean_var` | 2.37x | 0.00814 | 3.86 |
| `aalto_lin_std` | 2.36x | 0.00848 | 3.23 |
| `i319_pois_re2` | 2.35x | 0.0237 | 4.82 |
| `kidscore_interaction_z` | 2.35x | 0.0127 | 3.27 |
| `sw_re_bern` | 2.33x | 0.0117 | 4.66 |
| `aalto_lin` | 2.33x | 0.00792 | 3.05 |
| `ch13_m13_4b` | 2.32x | 0.0241 | 3.34 |
| `s2_cumulative_cloglog` | 2.32x | 0.0132 | 3.77 |
| `ch14_m14_3` | 2.31x | 0.0352 | 5.92 |
| `kidscore_mom_work` | 2.31x | 0.0131 | 3.12 |
| `accel_gp` | 2.31x | 0.0257 | 6.62 |
| `Mh_model` | 2.3x | 0.0478 | 3.36 |
| `ch15_m15_6` | 2.29x | 0.00851 | 3.19 |
| `s2_sar_error` | 2.28x | 0.0192 | 4.33 |
| `kidscore_interaction_c` | 2.27x | 0.0131 | 3.18 |
| `i320_mi_nhanes` | 2.26x | 0.0157 | 4.43 |
| `ch09_m9_5` | 2.26x | 0.00708 | 2.56 |
| `normal_mixture` | 2.25x | 0.0952 | 3.01 |
| `sw_vonmises` | 2.24x | 0.011 | 3.65 |
| `logearn_height_male` | 2.24x | 0.0184 | 3.06 |
| `ch13_m13_6` | 2.23x | 0.028 | 3.41 |
| `losscurve_sislob` | 2.23x | 0.0166 | 4.71 |
| `extra_hurdle_poisson` | 2.23x | 0.00736 | 2.59 |
| `s2_me2` | 2.23x | 0.0162 | 6.68 |
| `ch09_m9_4` | 2.22x | 0.00663 | 2.58 |
| `ch14_m14_5` | 2.22x | 0.0122 | 3.04 |
| `kidscore_interaction` | 2.22x | 0.0127 | 3.21 |
| `sw_lognormal` | 2.21x | 0.0107 | 3.63 |
| `kidscore_momhsiq` | 2.21x | 0.0111 | 3.1 |
| `low_dim_gauss_mix` | 2.2x | 0.106 | 3.43 |
| `dugongs_model` | 2.2x | 0.0104 | 3.03 |
| `ch13_m13_4` | 2.2x | 0.0279 | 3.43 |
| `ch11_m11_5` | 2.19x | 0.0272 | 3.13 |
| `s2_sar` | 2.19x | 0.0187 | 4.24 |
| `logearn_interaction_z` | 2.18x | 0.023 | 3.32 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.16x | 0.103 | 3.32 |
| `GLMM_Poisson_model` | 2.15x | 0.00969 | 3.85 |
| `dogs_hierarchical` | 2.15x | 0.0558 | 2.86 |
| `s2_gev` | 2.14x | 0.0152 | 3.84 |
| `ch09_m9_3` | 2.14x | 0.00685 | 2.52 |
| `s2_mi_lognormal` | 2.12x | 0.0141 | 4.56 |
| `logearn_logheight_male` | 2.12x | 0.02 | 3.09 |
| `logearn_interaction` | 2.11x | 0.0241 | 3.18 |
| `s2_mv_shared_re` | 2.09x | 0.0204 | 7.17 |
| `ldaK2` | 2.09x | 0.117 | 3.88 |
| `accel_splines` | 2.08x | 0.0207 | 4.59 |
| `ch13_m13_4nc` | 2.07x | 0.03 | 3.37 |
| `ch09_m9_2` | 2.06x | 0.00683 | 2.54 |
| `sw_nonlinear` | 2.06x | 0.0112 | 3.89 |
| `ch14_m14_4x` | 2.05x | 0.0104 | 2.87 |
| `log10earn_height` | 2.04x | 0.0146 | 2.89 |
| `sesame_one_pred_a` | 2.04x | 0.00933 | 2.83 |
| `earn_height` | 2.04x | 0.0141 | 2.86 |
| `sw_se` | 2.04x | 0.00926 | 3.44 |
| `sw_re_slope` | 2.04x | 0.016 | 6.83 |
| `kidscore_momhs` | 2.04x | 0.0102 | 2.92 |
| `ch13_m13_5` | 2.03x | 0.0248 | 3.28 |
| `seeds_centered_model` | 2.03x | 0.00965 | 3.91 |
| `ctsem_ctsm_400` | 2.02x | 22.1 | 501 |
| `s2_s_by` | 2.02x | 0.0236 | 5.74 |
| `aalto_lin_std_t` | 2.01x | 0.0091 | 3.46 |
| `GLM_Poisson_model` | 2x | 0.00831 | 3.3 |
| `sw_student` | 1.99x | 0.0109 | 3.8 |
| `ctsem_ctsm` | 1.99x | 2.85 | 441 |
| `sw_spline_t2` | 1.99x | 0.0155 | 4.82 |
| `ch14_m14_4` | 1.99x | 0.0109 | 2.87 |
| `sw_dist_sigma` | 1.98x | 0.0112 | 3.79 |
| `ch11_m11_6` | 1.97x | 0.00837 | 3 |
| `logearn_height` | 1.97x | 0.0149 | 2.9 |
| `election88_full` | 1.96x | 0.491 | 6.04 |
| `sw_re_pois` | 1.96x | 0.0125 | 4.51 |
| `ch11_m11_4` | 1.95x | 0.0247 | 3.05 |
| `ch13_m13_1` | 1.95x | 0.00861 | 2.98 |
| `kidscore_momiq` | 1.95x | 0.00987 | 2.91 |
| `sw_zi_poisson` | 1.95x | 0.0154 | 3.39 |
| `s2_nl_noloop` | 1.94x | 0.0106 | 3.83 |
| `ch11_m11_10` | 1.94x | 0.00779 | 3.23 |
| `radon_pooled` | 1.94x | 0.0965 | 3.02 |
| `ch13_m13_7nc` | 1.9x | 0.00598 | 2.34 |
| `s2_cens_interval` | 1.9x | 0.0142 | 3.74 |
| `normal_mixture_k` | 1.89x | 0.446 | 4.2 |
| `ch16_m16_4` | 1.89x | 0.0103 | 3.35 |
| `ch09_mp` | 1.89x | 0.00618 | 2.32 |
| `ch13_m13_3` | 1.88x | 0.00897 | 3.01 |
| `soil_incubation` | 1.86x | 0.0726 | 4.03 |
| `s2_index_mi` | 1.84x | 0.0137 | 4.17 |
| `lotka_volterra` | 1.84x | 0.0565 | 4.55 |
| `ch13_m13_2` | 1.84x | 0.00903 | 3.12 |
| `multi_occupancy` | 1.83x | 0.0858 | 6.56 |
| `s2_t2_by` | 1.82x | 0.0207 | 5.68 |
| `ch13_m13_7` | 1.82x | 0.00627 | 2.33 |
| `ch11_m11_8` | 1.81x | 0.0079 | 2.92 |
| `ch15_m15_3` | 1.77x | 0.0358 | 2.7 |
| `sw_zi_binomial` | 1.76x | 0.0177 | 3.43 |
| `sw_mi` | 1.76x | 0.0128 | 3.83 |
| `i319_pois_re` | 1.75x | 0.019 | 4.52 |
| `cm_loggamma` | 1.75x | 0.0434 | 4.65 |
| `sw_hurdle_gamma` | 1.75x | 0.0177 | 3.54 |
| `sw_asymlaplace` | 1.73x | 0.0152 | 3.64 |
| `cm_logweibull` | 1.73x | 0.0351 | 4.09 |
| `bym2_offset_only` | 1.72x | 0.0847 | 4.5 |
| `sw_ar` | 1.71x | 0.0147 | 4.25 |
| `2pl_latent_reg_irt` | 1.71x | 0.144 | 5.76 |
| `s2_threading` | 1.7x | 0.00994 | 3.38 |
| `ch15_m15_4` | 1.68x | 0.0304 | 2.76 |
| `hier_2pl` | 1.67x | 0.401 | 7.41 |
| `cm_lognormal` | 1.66x | 0.0771 | 5.52 |
| `ch11_m11_11` | 1.66x | 0.00854 | 3.55 |
| `cm_bisa` | 1.65x | 0.0366 | 4.16 |
| `sw_zi_negbin` | 1.65x | 0.0183 | 3.48 |
| `cm_choco` | 1.63x | 0.109 | 5.15 |
| `cm_weibull` | 1.62x | 0.0385 | 4.06 |
| `ch11_m11_7` | 1.62x | 0.00724 | 2.85 |
| `cm_invweibull` | 1.61x | 0.0395 | 4.07 |
| `cm_invgamma` | 1.61x | 0.0385 | 4.09 |
| `cm_lba1` | 1.61x | 0.0603 | 4.97 |
| `cm_betagate` | 1.6x | 0.0859 | 4.32 |
| `sw_arma` | 1.6x | 0.0169 | 4.34 |
| `seeds_stanified_model` | 1.59x | 0.0098 | 3.65 |
| `seeds_model` | 1.58x | 0.00997 | 3.73 |
| `sw_ma` | 1.57x | 0.0151 | 4.23 |
| `s2_categorical_re` | 1.56x | 0.0261 | 5.45 |
| `cm_gamma` | 1.56x | 0.0378 | 4.11 |
| `cm_logstudent` | 1.55x | 0.0413 | 4.22 |
| `surgical_model` | 1.54x | 0.00888 | 3.43 |
| `s2_zi_asymlaplace` | 1.52x | 0.0178 | 3.67 |
| `s2_unstr` | 1.52x | 0.0385 | 8.7 |
| `s2_mv_subset` | 1.51x | 0.0123 | 3.49 |
| `sw_gaussian` | 1.51x | 0.00977 | 3.31 |
| `sw_gamma` | 1.5x | 0.0108 | 3.76 |
| `sw_re_negbin` | 1.5x | 0.0156 | 4.76 |
| `prophet` | 1.49x | 0.0943 | 5.68 |
| `lsat_model` | 1.47x | 0.0966 | 3.86 |
| `covid19imperial_v3` | 1.46x | 1.31 | 7.6 |
| `gp_regr` | 1.46x | 0.0119 | 5.46 |
| `Mb_model` | 1.46x | 0.114 | 3.39 |
| `sw_cratio_cs` | 1.45x | 0.21 | 5.3 |
| `ch14_m14_2` | 1.45x | 0.0448 | 7.11 |
| `sw_mv_norescor` | 1.45x | 0.0128 | 3.49 |
| `cm_lnr_bench` | 1.44x | 0.57 | 8.96 |
| `gp_pois_regr` | 1.44x | 0.0116 | 5.64 |
| `cm_lba2` | 1.44x | 0.0868 | 7.45 |
| `s2_zoi_beta` | 1.43x | 0.0228 | 3.77 |
| `aalto_bern` | 1.43x | 0.00686 | 2.56 |
| `Rate_1_model` | 1.42x | 0.00582 | 2.37 |
| `aalto_binom` | 1.42x | 0.00621 | 2.37 |
| `Rate_4_model` | 1.42x | 0.0069 | 2.51 |
| `sw_cumulative_cs` | 1.42x | 0.185 | 5.17 |
| `Rate_2_model` | 1.41x | 0.00716 | 2.55 |
| `i320_sratio_cs` | 1.41x | 0.209 | 5.58 |
| `covid19imperial_v2` | 1.4x | 1.31 | 7.57 |
| `s2_rate` | 1.4x | 0.0101 | 3.6 |
| `GLM_Binomial_model` | 1.4x | 0.00944 | 3.28 |
| `cm_exwald` | 1.39x | 0.107 | 5.98 |
| `ch11_m11_9` | 1.39x | 0.00654 | 2.6 |
| `sw_exgaussian` | 1.39x | 0.0114 | 3.87 |
| `aalto_binom2` | 1.38x | 0.00671 | 2.5 |
| `i320_gp_expquad` | 1.38x | 0.0329 | 7.31 |
| `hmm_example` | 1.38x | 0.0571 | 4.14 |
| `s2_mixture_theta` | 1.38x | 0.0282 | 4.47 |
| `s2_cumulative_probit` | 1.37x | 0.018 | 3.69 |
| `ch14_m14_8nc` | 1.35x | 0.0158 | 5.99 |
| `s2_zi_beta` | 1.35x | 0.0211 | 3.72 |
| `aalto_binomb` | 1.34x | 0.00609 | 2.42 |
| `i319_negbin_re` | 1.34x | 0.0313 | 4.79 |
| `ch11_m_pois` | 1.33x | 0.00676 | 2.52 |
| `blr` | 1.33x | 0.00802 | 3.06 |
| `irt_2pl` | 1.33x | 0.0441 | 4.27 |
| `ch12_m12_1` | 1.32x | 0.0089 | 3.19 |
| `Rate_3_model` | 1.31x | 0.00644 | 2.4 |
| `wells_dist` | 1.3x | 0.0571 | 3.07 |
| `ch12_m12_2` | 1.3x | 0.00961 | 3.97 |
| `ch14_m14_6x` | 1.28x | 0.184 | 6.93 |
| `Rate_5_model` | 1.28x | 0.00696 | 2.49 |
| `ch12_m12_6` | 1.28x | 4.03 | 9.31 |
| `sw_gp` | 1.26x | 0.0504 | 7.32 |
| `ch14_m14_6` | 1.26x | 0.186 | 6.96 |
| `sw_mixture` | 1.25x | 0.0245 | 4.74 |
| `sw_bernoulli` | 1.24x | 0.00955 | 3.27 |
| `ch14_m14_10` | 1.23x | 0.749 | 5.15 |
| `sw_beta` | 1.23x | 0.013 | 3.9 |
| `ch14_m14_1` | 1.23x | 0.0197 | 6.9 |
| `ch14_m14_8` | 1.22x | 0.0156 | 4.77 |
| `nn_rbm1bJ10` | 1.22x | 0.345 | 5.85 |
| `ch14_m14_9` | 1.22x | 0.692 | 5.07 |
| `sw_mv_rescor` | 1.2x | 0.0255 | 6.11 |
| `s2_custom_vint` | 1.18x | 0.0204 | 3.54 |
| `sw_categorical` | 1.17x | 0.013 | 4.37 |
| `s2_frechet` | 1.17x | 0.0128 | 3.95 |
| `logistic_regression_rhs` | 1.16x | 0.108 | 5.14 |
| `cm_rdm` | 1.16x | 0.234 | 10.8 |
| `sw_weibull` | 1.15x | 0.0126 | 3.92 |
| `Survey_model` | 1.15x | 0.126 | 3.09 |
| `ch14_m14_7` | 1.15x | 0.0735 | 8.86 |
| `s2_multinomial` | 1.13x | 0.0296 | 3.95 |
| `sw_binomial` | 1.13x | 0.0118 | 3.49 |
| `i320_sratio_plain` | 1.13x | 0.137 | 4.18 |
| `sw_sratio` | 1.12x | 0.14 | 4.05 |
| `sw_negbinomial` | 1.11x | 0.012 | 3.41 |
| `cm_lnr` | 1.1x | 0.319 | 14 |
| `nn_rbm1bJ100` | 1.09x | 855 | 933 |
| `sw_poisson` | 1.09x | 0.00923 | 3.11 |
| `sw_cratio` | 1.08x | 0.142 | 4.04 |
| `sw_mono` | 1.08x | 0.0151 | 4.13 |
| `s2_hurdle_negbin` | 1.07x | 0.0262 | 3.51 |
| `aalto_poisson_simple` | 1.06x | 0.00962 | 2.67 |
| `s2_beta_binomial` | 1.06x | 0.0187 | 3.7 |
| `s2_weights_trunc` | 1.06x | 0.0234 | 3.59 |
| `wells_dist100_model` | 1.05x | 0.0426 | 3.23 |
| `s2_discrete_weibull` | 1.05x | 0.0172 | 3.51 |
| `bones_model` | 1.04x | 0.124 | 3.6 |
| `sw_trunc` | 1.04x | 0.0229 | 3.54 |
| `wells_interaction_model` | 1.04x | 0.0476 | 3.3 |
| `wells_dae_inter_model` | 1.03x | 0.051 | 3.4 |
| `hmm_gaussian` | 1.03x | 0.704 | 5.39 |
| `one_comp_mm_elim_abs` | 1.03x | 0.979 | 4.8 |
| `wells_dae_c_model` | 1.03x | 0.0472 | 3.41 |
| `hmm_drive_1` | 1.03x | 0.356 | 5.08 |
| `i319_pois_fixed` | 1.02x | 0.0138 | 3.13 |
| `wells_dae_model` | 1.02x | 0.0501 | 3.28 |
| `ch14_m14_11` | 1.02x | 1.42 | 5.82 |
| `s2_mo_simo_prior` | 1.01x | 0.0159 | 4.1 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 0.0482 | 3.36 |
| `s2_wiener` | 1.01x | 0.0714 | 3.75 |
| `s2_dirichlet` | 0.999x | 0.0362 | 4.39 |
| `sw_cumulative` | 0.998x | 0.0531 | 3.78 |
| `i319_negbin_fixed` | 0.998x | 0.0259 | 3.41 |
| `ch15_m15_7` | 0.996x | 0.0346 | 7.62 |
| `wells_daae_c_model` | 0.992x | 0.0513 | 3.42 |
| `s2_ar_cov` | 0.989x | 0.0319 | 7.61 |
| `nes_logit_model` | 0.984x | 0.0203 | 3.18 |
| `wells_dist100ars_model` | 0.983x | 0.0462 | 3.27 |
| `s2_logistic_normal` | 0.981x | 0.0544 | 6.73 |
| `diamonds` | 0.98x | 0.0924 | 3.49 |
| `i320_pois_trunc_both` | 0.977x | 0.0766 | 3.58 |
| `i320_pois_trunc_ub` | 0.975x | 0.0606 | 3.5 |
| `iohmm_reg` | 0.972x | 0.895 | 6.82 |
| `hmm_drive_0` | 0.971x | 0.343 | 5.1 |
| `s2_cosy` | 0.966x | 0.0301 | 6.72 |
| `cm_betadiscrete` | 0.961x | 3.99 | 8.12 |
| `s2_fcor` | 0.951x | 0.0378 | 4.89 |
| `cm_ddm` | 0.94x | 38 | 44.9 |
| `i320_gp_matern32` | 0.88x | 0.0757 | 7.23 |
| `garch11` | 0.847x | 0.0249 | 2.95 |
| `aalto_gpareto` | 0.818x | 0.0135 | 3.06 |
| `arma11` | 0.712x | 0.0192 | 2.98 |
| `cm_invgaussian` | 0.426x | 0.379 | 33.8 |
| `s2_com_poisson` | 0.213x | 0.496 | 4.76 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2797) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5360) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6075) |
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
`--build-jobs N` (default 8) sets how many fresh CmdStan builds run at once.
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
