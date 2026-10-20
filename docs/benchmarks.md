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

Each engine's setup time plus 5,000 times its
median warm gradient time. No sampler runs. Adaptation, tree building,
generated quantities and CSV output are left out, and a fixed gradient budget
says nothing about how long each engine needs to reach the same inference
quality. The run manifests record a 20,000-gradient budget; the tables
recompute totals for 5,000 gradients from the same measurements. A missing
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
| `ch12_m12_4` | 32.7x | 0.362 | 14.2 |
| `gpcm_latent_reg_irt` | 12.6x | 0.675 | 12.3 |
| `grsm_latent_reg_irt` | 11.9x | 0.376 | 9.08 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 0.168 | 4.42 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 0.176 | 4.68 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.2x | 0.277 | 5.44 |
| `radon_county_intercept` | 10.2x | 0.278 | 5.26 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.84x | 0.284 | 5.58 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.82x | 0.284 | 5.36 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.82x | 0.284 | 5.55 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.31x | 0.308 | 5.53 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.31x | 0.418 | 6.47 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.16x | 0.405 | 6.19 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 8.91x | 0.322 | 5.76 |
| `radon_pooled` | 8.29x | 0.217 | 4.46 |
| `dogs` | 7.99x | 0.0676 | 3.96 |
| `aalto_poisson_hurdle` | 7.81x | 0.0642 | 3.24 |
| `s2_custom_vreal` | 7.73x | 0.0104 | 3.51 |
| `rats_model` | 7.45x | 0.0128 | 3.12 |
| `arK` | 7.24x | 0.0174 | 2.94 |
| `ch12_m12_3` | 6.73x | 0.0224 | 2.59 |
| `ch12_m12_3_alt` | 6.61x | 0.0218 | 2.61 |
| `ch16_m16_1` | 6.54x | 0.0325 | 3.15 |
| `M0_model` | 6.29x | 0.0223 | 2.79 |
| `Mt_model` | 5.72x | 0.028 | 3.55 |
| `sw_nonlinear` | 5.36x | 0.0118 | 3.9 |
| `ctsem_ctsm_400` | 5.13x | 20 | 187 |
| `ctsem_ctsm` | 4.98x | 2.69 | 98.1 |
| `ch09_m5_8s` | 4.85x | 0.01 | 2.89 |
| `ch14_m14_5` | 4.66x | 0.0189 | 2.92 |
| `s2_cumulative_cauchit` | 4.63x | 0.018 | 3.71 |
| `ch09_m5_8s2` | 4.59x | 0.00971 | 2.88 |
| `ch15_m15_1` | 4.41x | 0.0105 | 3.21 |
| `logmesquite` | 4.3x | 0.0113 | 3.18 |
| `logmesquite_logvas` | 4.26x | 0.0108 | 3.38 |
| `mesquite` | 4.25x | 0.0108 | 3.09 |
| `logmesquite_logvash` | 4.24x | 0.0103 | 3.36 |
| `s2_nl_noloop` | 4.21x | 0.011 | 3.94 |
| `logmesquite_logvolume` | 4.14x | 0.00824 | 3.03 |
| `election88_full` | 4.05x | 1.19 | 8.96 |
| `s2_logistic_normal` | 4.02x | 0.0422 | 6.41 |
| `ch15_m15_2` | 4.01x | 0.0117 | 3.44 |
| `s2_car_icar` | 3.99x | 0.0131 | 4.42 |
| `nes` | 3.99x | 0.103 | 3.6 |
| `logmesquite_logva` | 3.99x | 0.0094 | 3.17 |
| `Mth_model` | 3.88x | 0.146 | 4.79 |
| `sw_spline_s` | 3.84x | 0.0126 | 4.49 |
| `sw_me` | 3.8x | 0.0128 | 4.15 |
| `ch14_m14_4` | 3.76x | 0.015 | 2.82 |
| `ch14_m14_4x` | 3.76x | 0.0154 | 2.82 |
| `sw_dist_sigma` | 3.74x | 0.0142 | 3.87 |
| `ch09_m9_1_chains4` | 3.74x | 0.0102 | 3.12 |
| `logearn_logheight_male` | 3.72x | 0.0348 | 3.05 |
| `s2_s_cc` | 3.67x | 0.0109 | 4.32 |
| `s2_dist_sigma_re` | 3.65x | 0.0142 | 4.93 |
| `s2_me2_nomecor` | 3.64x | 0.017 | 4.44 |
| `kidscore_interaction_c2` | 3.62x | 0.0204 | 3.08 |
| `s2_nlf` | 3.61x | 0.0128 | 4 |
| `kidscore_interaction` | 3.54x | 0.021 | 3.09 |
| `sw_weights` | 3.54x | 0.0118 | 3.54 |
| `ch09_m9_1` | 3.53x | 0.0112 | 3.12 |
| `kidscore_interaction_c` | 3.51x | 0.0213 | 3.08 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.45x | 0.041 | 4.98 |
| `s2_mm_weights` | 3.44x | 0.0151 | 4.68 |
| `logearn_height_male` | 3.43x | 0.0349 | 3.02 |
| `s2_mm` | 3.42x | 0.0143 | 4.68 |
| `kidscore_mom_work` | 3.42x | 0.022 | 2.99 |
| `kidscore_interaction_z` | 3.41x | 0.0209 | 3.12 |
| `kilpisjarvi` | 3.39x | 0.00848 | 2.82 |
| `logearn_interaction_z` | 3.38x | 0.0465 | 3.26 |
| `s2_gp_approx` | 3.36x | 0.0153 | 5.22 |
| `GLMM1_model` | 3.35x | 0.0612 | 3.43 |
| `sw_spline_t2` | 3.35x | 0.0178 | 4.94 |
| `sw_se` | 3.35x | 0.00983 | 3.55 |
| `s2_cox_cens` | 3.35x | 0.017 | 5.1 |
| `ch16_m16_4` | 3.35x | 0.0155 | 2.92 |
| `s2_car` | 3.33x | 0.0155 | 4.41 |
| `logearn_interaction` | 3.32x | 0.0479 | 3.16 |
| `aalto_grp_aov` | 3.29x | 0.00779 | 3.08 |
| `sw_lognormal` | 3.28x | 0.0104 | 3.72 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.26x | 0.00875 | 3.29 |
| `s2_s_by` | 3.25x | 0.0271 | 5.92 |
| `s2_car_esicar` | 3.23x | 0.0158 | 4.5 |
| `s2_gr_by` | 3.23x | 0.0137 | 4.95 |
| `s2_shifted_lognormal` | 3.22x | 0.0118 | 3.93 |
| `s2_gp_by_approx` | 3.21x | 0.0226 | 6.04 |
| `i319_gauss_re` | 3.19x | 0.0189 | 4.59 |
| `ch15_m15_8` | 3.19x | 0.0197 | 2.9 |
| `kidscore_momhsiq` | 3.18x | 0.0171 | 2.98 |
| `ch15_m15_9` | 3.17x | 0.0208 | 3.05 |
| `sw_acat` | 3.11x | 0.254 | 4.62 |
| `s2_t2_by` | 3.1x | 0.0249 | 5.84 |
| `radon_county` | 3.08x | 0.143 | 3.55 |
| `ch15_m15_5` | 3.08x | 0.011 | 3.25 |
| `s2_cox` | 3.01x | 0.0146 | 4.19 |
| `aalto_grp_prior_mean` | 2.99x | 0.00793 | 3.16 |
| `s2_mi_lognormal` | 2.95x | 0.0143 | 4.62 |
| `s2_cumulative_cloglog` | 2.94x | 0.0159 | 3.7 |
| `sw_re_gauss` | 2.94x | 0.0135 | 4.57 |
| `log10earn_height` | 2.93x | 0.0259 | 2.89 |
| `bym2_offset_only` | 2.91x | 0.199 | 4.76 |
| `logearn_height` | 2.89x | 0.026 | 2.89 |
| `earn_height` | 2.87x | 0.0257 | 2.88 |
| `ch15_m15_6` | 2.86x | 0.0089 | 2.98 |
| `Mtbh_model` | 2.85x | 0.102 | 5.21 |
| `dogs_nonhierarchical` | 2.85x | 0.104 | 7.15 |
| `sesame_one_pred_a` | 2.84x | 0.0116 | 2.83 |
| `pilots` | 2.83x | 0.0109 | 3.41 |
| `GLMM_Poisson_model` | 2.82x | 0.0112 | 3.84 |
| `sw_student` | 2.79x | 0.0115 | 3.88 |
| `s2_mmc` | 2.77x | 0.0208 | 6.96 |
| `ch13_m13_4nc` | 2.77x | 0.0544 | 3.43 |
| `kidscore_momiq` | 2.76x | 0.0144 | 2.89 |
| `sw_hurdle_lognormal` | 2.76x | 0.0183 | 3.59 |
| `cm_geg` | 2.75x | 0.136 | 4.19 |
| `aalto_lin_std` | 2.75x | 0.00865 | 3.32 |
| `s2_hurdle_cumulative` | 2.73x | 0.0253 | 4.2 |
| `blr` | 2.72x | 0.00859 | 3.16 |
| `s2_gr_student` | 2.7x | 0.0158 | 5.2 |
| `cm_exgaussian` | 2.7x | 0.0846 | 3.84 |
| `hierarchical_gp` | 2.68x | 0.112 | 9.29 |
| `aalto_lin` | 2.67x | 0.00812 | 3.1 |
| `eight_schools_centered` | 2.66x | 0.00747 | 3 |
| `aalto_grp_prior_mean_var` | 2.65x | 0.00916 | 3.93 |
| `ch12_m12_7` | 2.64x | 5.01 | 16.7 |
| `GLM_Poisson_model` | 2.64x | 0.00931 | 3.32 |
| `sw_gamma` | 2.63x | 0.0114 | 3.76 |
| `kidscore_momhs` | 2.63x | 0.0139 | 2.9 |
| `accel_splines` | 2.6x | 0.0314 | 4.45 |
| `ch12_m12_5` | 2.58x | 5.2 | 16.8 |
| `sw_hurdle_pois` | 2.57x | 0.0201 | 3.43 |
| `accel_gp` | 2.55x | 0.0376 | 5.52 |
| `s2_me2` | 2.53x | 0.0209 | 6.72 |
| `ldaK5` | 2.53x | 12 | 33.3 |
| `s2_gev` | 2.51x | 0.0206 | 3.8 |
| `s2_sar` | 2.48x | 0.0286 | 4.12 |
| `s2_sar_error` | 2.47x | 0.0291 | 4.18 |
| `sw_acat_cs` | 2.46x | 0.362 | 5.85 |
| `sw_skewnormal` | 2.46x | 0.0141 | 3.96 |
| `losscurve_sislob` | 2.43x | 0.0179 | 4.33 |
| `ch14_m14_3` | 2.42x | 0.0604 | 6 |
| `i319_pois_re2` | 2.4x | 0.0388 | 4.89 |
| `s2_rate` | 2.4x | 0.0122 | 3.66 |
| `ch15_m15_3` | 2.4x | 0.0715 | 2.8 |
| `logistic_regression_rhs` | 2.39x | 0.243 | 5.46 |
| `Mh_model` | 2.38x | 0.0946 | 3.51 |
| `ch15_m15_4` | 2.38x | 0.0614 | 2.76 |
| `seeds_centered_model` | 2.36x | 0.0121 | 3.86 |
| `s2_mv_shared_re` | 2.35x | 0.0251 | 7.1 |
| `ch11_m11_10` | 2.29x | 0.00861 | 3.24 |
| `sw_re_bern` | 2.28x | 0.0146 | 4.64 |
| `hier_2pl` | 2.27x | 0.954 | 8.97 |
| `sw_vonmises` | 2.25x | 0.012 | 3.72 |
| `i320_mi_nhanes` | 2.24x | 0.0178 | 4.43 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.23x | 0.238 | 3.64 |
| `extra_hurdle_poisson` | 2.23x | 0.00779 | 2.63 |
| `dogs_hierarchical` | 2.2x | 0.102 | 2.97 |
| `ch13_m13_4` | 2.2x | 0.0475 | 3.5 |
| `ch09_m9_2` | 2.2x | 0.00692 | 2.55 |
| `ldaK2` | 2.18x | 0.271 | 4.3 |
| `ch13_m13_4b` | 2.18x | 0.044 | 3.39 |
| `ch09_m9_3` | 2.17x | 0.00665 | 2.51 |
| `lsat_model` | 2.17x | 0.227 | 4.23 |
| `sw_zi_poisson` | 2.15x | 0.0177 | 3.42 |
| `ch09_m9_5` | 2.15x | 0.00695 | 2.59 |
| `2pl_latent_reg_irt` | 2.15x | 0.346 | 6.56 |
| `sw_re_slope` | 2.15x | 0.0183 | 6.79 |
| `dugongs_model` | 2.14x | 0.0101 | 3.03 |
| `normal_mixture` | 2.14x | 0.224 | 3.07 |
| `ch09_m9_4` | 2.11x | 0.0068 | 2.57 |
| `ch11_m11_5` | 2.11x | 0.0463 | 3.18 |
| `ch13_m13_6` | 2.11x | 0.0482 | 3.48 |
| `aalto_lin_std_t` | 2.11x | 0.00973 | 3.5 |
| `sw_re_pois` | 2.1x | 0.0146 | 4.52 |
| `irt_2pl` | 2.07x | 0.095 | 4.32 |
| `multi_occupancy` | 2.06x | 0.176 | 6.35 |
| `low_dim_gauss_mix` | 2.05x | 0.26 | 3.72 |
| `prophet` | 2.04x | 0.204 | 5.3 |
| `s2_index_mi` | 2.01x | 0.0169 | 4.22 |
| `ch11_m11_4` | 1.98x | 0.0424 | 3.09 |
| `ch11_m11_6` | 1.98x | 0.00932 | 3.03 |
| `seeds_model` | 1.98x | 0.0117 | 3.68 |
| `ch13_m13_7` | 1.98x | 0.0066 | 2.32 |
| `soil_incubation` | 1.97x | 0.17 | 3.78 |
| `seeds_stanified_model` | 1.96x | 0.0116 | 3.59 |
| `normal_mixture_k` | 1.96x | 1.04 | 5.4 |
| `sw_exgaussian` | 1.95x | 0.0135 | 3.89 |
| `ch11_m11_8` | 1.92x | 0.00855 | 2.94 |
| `ch13_m13_5` | 1.89x | 0.0434 | 3.33 |
| `s2_cens_interval` | 1.89x | 0.0184 | 3.81 |
| `ch13_m13_1` | 1.88x | 0.011 | 3 |
| `ch13_m13_7nc` | 1.88x | 0.00623 | 2.36 |
| `ch09_mp` | 1.86x | 0.00691 | 2.31 |
| `i319_pois_re` | 1.83x | 0.0293 | 4.54 |
| `ch13_m13_3` | 1.83x | 0.0115 | 3.03 |
| `sw_ar` | 1.81x | 0.0184 | 4.27 |
| `sw_arma` | 1.8x | 0.0229 | 4.38 |
| `sw_zi_binomial` | 1.79x | 0.0232 | 3.45 |
| `ch13_m13_2` | 1.79x | 0.0117 | 3.14 |
| `sw_mi` | 1.79x | 0.0152 | 3.87 |
| `lotka_volterra` | 1.78x | 0.129 | 4.57 |
| `sw_hurdle_gamma` | 1.76x | 0.0236 | 3.58 |
| `cm_lognormal` | 1.75x | 0.126 | 4.38 |
| `s2_threading` | 1.75x | 0.0112 | 3.39 |
| `cm_choco` | 1.74x | 0.242 | 4.88 |
| `sw_ma` | 1.73x | 0.019 | 4.26 |
| `sw_asymlaplace` | 1.71x | 0.0209 | 3.57 |
| `wells_dist` | 1.7x | 0.13 | 3.18 |
| `sw_beta` | 1.69x | 0.0183 | 3.93 |
| `cm_invweibull` | 1.69x | 0.076 | 4.15 |
| `ch11_m11_11` | 1.68x | 0.00979 | 3.57 |
| `cm_bisa` | 1.68x | 0.0699 | 4.11 |
| `cm_logweibull` | 1.67x | 0.0679 | 4.1 |
| `cm_loggamma` | 1.67x | 0.0823 | 4.29 |
| `s2_mv_subset` | 1.66x | 0.014 | 3.51 |
| `cm_weibull` | 1.65x | 0.0741 | 4.12 |
| `sw_gaussian` | 1.64x | 0.00983 | 3.32 |
| `GLM_Binomial_model` | 1.64x | 0.0116 | 3.3 |
| `cm_logstudent` | 1.63x | 0.0785 | 4.26 |
| `cm_invgamma` | 1.62x | 0.0787 | 4.14 |
| `cm_gamma` | 1.62x | 0.0751 | 4.12 |
| `sw_zi_negbin` | 1.62x | 0.0269 | 3.5 |
| `surgical_model` | 1.59x | 0.00977 | 3.46 |
| `cm_lnr_bench` | 1.59x | 0.959 | 5.9 |
| `s2_zoi_beta` | 1.57x | 0.0344 | 3.7 |
| `s2_zi_beta` | 1.57x | 0.0333 | 3.69 |
| `cm_betagate` | 1.57x | 0.188 | 4.27 |
| `ch11_m11_7` | 1.57x | 0.00746 | 2.85 |
| `sw_re_negbin` | 1.54x | 0.0203 | 4.76 |
| `s2_unstr` | 1.52x | 0.0492 | 7.5 |
| `gp_pois_regr` | 1.52x | 0.0177 | 5.79 |
| `cm_lba1` | 1.51x | 0.121 | 4.49 |
| `sw_mv_norescor` | 1.51x | 0.0138 | 3.48 |
| `covid19imperial_v2` | 1.51x | 2.05 | 8.75 |
| `s2_categorical_re` | 1.51x | 0.0408 | 5.46 |
| `gp_regr` | 1.49x | 0.0191 | 5.49 |
| `s2_frechet` | 1.49x | 0.019 | 3.99 |
| `sw_weibull` | 1.48x | 0.018 | 3.92 |
| `Rate_4_model` | 1.47x | 0.00717 | 2.53 |
| `Mb_model` | 1.47x | 0.218 | 3.53 |
| `s2_gp_by_gr` | 1.46x | 0.0622 | 9.18 |
| `s2_mixture_theta` | 1.46x | 0.0454 | 4.62 |
| `covid19imperial_v3` | 1.45x | 2.03 | 8.7 |
| `aalto_binom` | 1.45x | 0.00609 | 2.39 |
| `s2_zi_asymlaplace` | 1.45x | 0.0275 | 3.61 |
| `hmm_example` | 1.43x | 0.116 | 4.23 |
| `ch12_m12_6` | 1.43x | 9.88 | 18.2 |
| `aalto_bern` | 1.42x | 0.00659 | 2.57 |
| `Rate_1_model` | 1.42x | 0.00637 | 2.39 |
| `ch15_m15_7` | 1.4x | 0.0553 | 7.19 |
| `i320_gp_expquad` | 1.39x | 0.0644 | 7.1 |
| `sw_binomial` | 1.39x | 0.0173 | 3.59 |
| `cm_exwald` | 1.39x | 0.17 | 4.41 |
| `ch11_m11_9` | 1.37x | 0.00732 | 2.61 |
| `Rate_5_model` | 1.36x | 0.00714 | 2.51 |
| `cm_lba2` | 1.36x | 0.157 | 4.82 |
| `aalto_binom2` | 1.36x | 0.00673 | 2.5 |
| `ch14_m14_2` | 1.36x | 0.096 | 7.18 |
| `aalto_binomb` | 1.34x | 0.00642 | 2.42 |
| `sw_mono` | 1.34x | 0.0202 | 4 |
| `i319_negbin_re` | 1.34x | 0.0584 | 4.83 |
| `ch12_m12_2` | 1.33x | 0.0122 | 3.99 |
| `ch12_m12_1` | 1.32x | 0.0126 | 3.21 |
| `ch14_m14_8nc` | 1.31x | 0.0249 | 5.72 |
| `Rate_3_model` | 1.3x | 0.00724 | 2.39 |
| `Rate_2_model` | 1.3x | 0.00758 | 2.55 |
| `ch14_m14_1` | 1.29x | 0.0353 | 6.91 |
| `ch11_m_pois` | 1.28x | 0.00673 | 2.54 |
| `ch14_m14_6x` | 1.28x | 0.445 | 7.17 |
| `s2_mo_simo_prior` | 1.27x | 0.0204 | 4 |
| `sw_mixture` | 1.27x | 0.0403 | 4.84 |
| `sw_gp` | 1.27x | 0.107 | 7.17 |
| `nn_rbm1bJ10` | 1.26x | 0.805 | 6.5 |
| `s2_beta_binomial` | 1.25x | 0.0316 | 3.74 |
| `ch14_m14_6` | 1.25x | 0.439 | 7.17 |
| `cm_rdm` | 1.24x | 0.315 | 4.64 |
| `ch14_m14_9` | 1.24x | 1.71 | 6.2 |
| `sw_mv_rescor` | 1.24x | 0.0419 | 6.21 |
| `sw_cratio` | 1.24x | 0.318 | 4.01 |
| `sw_sratio` | 1.23x | 0.31 | 3.97 |
| `ch14_m14_10` | 1.23x | 1.85 | 6.36 |
| `i320_sratio_plain` | 1.22x | 0.314 | 4.14 |
| `sw_bernoulli` | 1.21x | 0.00998 | 3.26 |
| `s2_custom_vint` | 1.19x | 0.0356 | 3.57 |
| `ch14_m14_7` | 1.18x | 0.158 | 9 |
| `ch14_m14_8` | 1.17x | 0.0241 | 4.51 |
| `s2_cumulative_probit` | 1.17x | 0.0304 | 3.74 |
| `sw_categorical` | 1.17x | 0.0151 | 4.34 |
| `cm_lnr` | 1.15x | 0.39 | 4.75 |
| `sw_cratio_cs` | 1.15x | 0.469 | 5.26 |
| `sw_poisson` | 1.14x | 0.0112 | 3.15 |
| `Survey_model` | 1.13x | 0.298 | 3.29 |
| `s2_multinomial` | 1.12x | 0.0574 | 4.05 |
| `i320_sratio_cs` | 1.12x | 0.475 | 5.53 |
| `ch14_m14_11` | 1.11x | 3.13 | 7.43 |
| `s2_discrete_weibull` | 1.11x | 0.0267 | 3.5 |
| `s2_hurdle_negbin` | 1.1x | 0.0461 | 3.51 |
| `sw_negbinomial` | 1.08x | 0.0163 | 3.37 |
| `nn_rbm1bJ100` | 1.06x | 2.16e+03 | 2.3e+03 |
| `s2_weights_trunc` | 1.06x | 0.0427 | 3.65 |
| `wells_dist100_model` | 1.05x | 0.0935 | 3.28 |
| `s2_dirichlet` | 1.04x | 0.0723 | 4.43 |
| `aalto_poisson_simple` | 1.04x | 0.0122 | 2.65 |
| `s2_wiener` | 1.04x | 0.155 | 3.84 |
| `i319_negbin_fixed` | 1.03x | 0.0496 | 3.44 |
| `wells_dist100ars_model` | 1.03x | 0.103 | 3.31 |
| `garch11` | 1.02x | 0.0507 | 3.02 |
| `hmm_drive_1` | 1.02x | 0.813 | 5.59 |
| `wells_dae_c_model` | 1.02x | 0.103 | 3.45 |
| `sw_trunc` | 1.02x | 0.0406 | 3.6 |
| `iohmm_reg` | 1.02x | 1.88 | 7.71 |
| `sw_cumulative` | 1.02x | 0.115 | 3.9 |
| `wells_dae_model` | 1.02x | 0.111 | 3.32 |
| `hmm_gaussian` | 1.02x | 1.48 | 6.16 |
| `wells_dae_inter_model` | 1.02x | 0.113 | 3.43 |
| `arma11` | 1.01x | 0.0361 | 3.01 |
| `diamonds` | 1.01x | 0.187 | 3.61 |
| `i320_pois_trunc_both` | 1.01x | 0.173 | 3.74 |
| `s2_ar_cov` | 1.01x | 0.0505 | 7.22 |
| `nes_logit_model` | 1.01x | 0.0403 | 3.18 |
| `i320_pois_trunc_ub` | 1.01x | 0.138 | 3.62 |
| `bones_model` | 0.998x | 0.284 | 3.79 |
| `sw_cumulative_cs` | 0.998x | 0.369 | 5.09 |
| `wells_interaction_model` | 0.997x | 0.111 | 3.34 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.995x | 0.11 | 3.45 |
| `cm_ddm` | 0.992x | 96.6 | 101 |
| `cm_betadiscrete` | 0.987x | 9.86 | 13.9 |
| `i319_pois_fixed` | 0.986x | 0.0229 | 3.14 |
| `wells_daae_c_model` | 0.982x | 0.114 | 3.48 |
| `one_comp_mm_elim_abs` | 0.982x | 2.48 | 5.85 |
| `s2_cosy` | 0.964x | 0.0477 | 6.49 |
| `hmm_drive_0` | 0.951x | 0.78 | 5.49 |
| `i320_gp_matern32` | 0.932x | 0.166 | 7.07 |
| `s2_fcor` | 0.93x | 0.0797 | 4.97 |
| `aalto_gpareto` | 0.836x | 0.015 | 2.89 |
| `cm_invgaussian` | 0.763x | 0.446 | 4.91 |
| `s2_com_poisson` | 0.253x | 1.21 | 3.94 |

Total sampling is an estimate: each engine's measured setup time + 5,000 × its median warm gradient time. Full sampling is not run.

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
| `ch12_m12_4` | 31.8x | 0.362 | 14.1 |
| `gpcm_latent_reg_irt` | 12.6x | 0.675 | 12.7 |
| `grsm_latent_reg_irt` | 12.1x | 0.376 | 9.4 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.5x | 0.168 | 4.51 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.8x | 0.176 | 4.67 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.29x | 0.277 | 4.92 |
| `radon_county_intercept` | 8.24x | 0.278 | 4.84 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.19x | 0.284 | 4.94 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 7.83x | 0.284 | 5.09 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.74x | 0.284 | 5.05 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.64x | 0.308 | 5.07 |
| `aalto_poisson_hurdle` | 7.51x | 0.0642 | 3.25 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.41x | 0.405 | 5.63 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.37x | 0.322 | 5.38 |
| `s2_custom_vreal` | 7.18x | 0.0104 | 3.45 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.1x | 0.418 | 5.81 |
| `ch12_m12_3_alt` | 6.79x | 0.0218 | 2.6 |
| `ch12_m12_3` | 6.67x | 0.0224 | 2.57 |
| `ch16_m16_1` | 6.62x | 0.0325 | 3.12 |
| `rats_model` | 6.51x | 0.0128 | 3.11 |
| `M0_model` | 6.4x | 0.0223 | 2.78 |
| `ctsem_ctsm_400` | 5.61x | 20 | 566 |
| `ctsem_ctsm` | 5.38x | 2.69 | 447 |
| `arK` | 4.91x | 0.0174 | 2.91 |
| `Mt_model` | 4.84x | 0.028 | 3.56 |
| `Mth_model` | 4.07x | 0.146 | 4.8 |
| `s2_car_icar` | 4.03x | 0.0131 | 4.39 |
| `s2_cumulative_cauchit` | 4.01x | 0.018 | 3.76 |
| `s2_logistic_normal` | 3.58x | 0.0422 | 6.78 |
| `s2_mm_weights` | 3.51x | 0.0151 | 4.71 |
| `dogs` | 3.49x | 0.0676 | 4.32 |
| `sw_weights` | 3.39x | 0.0118 | 3.46 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.34x | 0.041 | 4.92 |
| `GLMM1_model` | 3.34x | 0.0612 | 3.37 |
| `s2_car` | 3.32x | 0.0155 | 4.71 |
| `cm_exgaussian` | 3.31x | 0.0846 | 4.56 |
| `sw_me` | 3.3x | 0.0128 | 4.28 |
| `s2_cox_cens` | 3.29x | 0.017 | 4.99 |
| `s2_mm` | 3.26x | 0.0143 | 4.71 |
| `s2_car_esicar` | 3.22x | 0.0158 | 4.79 |
| `i319_gauss_re` | 3.21x | 0.0189 | 4.59 |
| `ch09_m9_1_chains4` | 3.19x | 0.0102 | 3.33 |
| `s2_me2_nomecor` | 3.15x | 0.017 | 4.54 |
| `s2_gp_by_approx` | 3.14x | 0.0226 | 6.51 |
| `logmesquite_logvash` | 3.14x | 0.0103 | 3.54 |
| `s2_gr_by` | 3.08x | 0.0137 | 4.94 |
| `sw_re_gauss` | 3.07x | 0.0135 | 4.59 |
| `mesquite` | 3.07x | 0.0108 | 3.38 |
| `s2_dist_sigma_re` | 3.05x | 0.0142 | 4.96 |
| `logmesquite_logva` | 3.03x | 0.0094 | 3.33 |
| `ch15_m15_1` | 3.03x | 0.0105 | 3.42 |
| `ch09_m9_1` | 3.02x | 0.0112 | 3.29 |
| `logmesquite` | 2.99x | 0.0113 | 3.42 |
| `aalto_grp_aov` | 2.98x | 0.00779 | 3.05 |
| `radon_county` | 2.97x | 0.143 | 3.52 |
| `logmesquite_logvolume` | 2.95x | 0.00824 | 3.03 |
| `logmesquite_logvas` | 2.92x | 0.0108 | 3.65 |
| `s2_cox` | 2.91x | 0.0146 | 4.12 |
| `cm_geg` | 2.83x | 0.136 | 4.86 |
| `s2_hurdle_cumulative` | 2.82x | 0.0253 | 4.2 |
| `eight_schools_centered` | 2.8x | 0.00747 | 2.99 |
| `aalto_grp_prior_mean` | 2.77x | 0.00793 | 3.14 |
| `s2_gr_student` | 2.77x | 0.0158 | 5.3 |
| `sw_acat` | 2.75x | 0.254 | 4.6 |
| `Mtbh_model` | 2.72x | 0.102 | 5.12 |
| `sw_acat_cs` | 2.71x | 0.362 | 5.99 |
| `ch15_m15_5` | 2.7x | 0.011 | 3.69 |
| `dogs_nonhierarchical` | 2.7x | 0.104 | 7.02 |
| `sw_hurdle_lognormal` | 2.67x | 0.0183 | 3.55 |
| `ch09_m5_8s` | 2.66x | 0.01 | 3.04 |
| `hierarchical_gp` | 2.63x | 0.112 | 9.29 |
| `ch15_m15_9` | 2.61x | 0.0208 | 3.16 |
| `s2_mmc` | 2.59x | 0.0208 | 7.03 |
| `ch15_m15_2` | 2.58x | 0.0117 | 3.65 |
| `sw_hurdle_pois` | 2.58x | 0.0201 | 3.43 |
| `s2_s_cc` | 2.55x | 0.0109 | 4.21 |
| `ch15_m15_8` | 2.55x | 0.0197 | 2.92 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.52x | 0.00875 | 3.21 |
| `ch09_m5_8s2` | 2.5x | 0.00971 | 3.04 |
| `ldaK5` | 2.48x | 12 | 32.6 |
| `nes` | 2.48x | 0.103 | 3.84 |
| `kilpisjarvi` | 2.47x | 0.00848 | 2.8 |
| `ch12_m12_7` | 2.46x | 5.01 | 15.6 |
| `s2_gp_approx` | 2.44x | 0.0153 | 5.52 |
| `aalto_grp_prior_mean_var` | 2.44x | 0.00916 | 3.86 |
| `s2_shifted_lognormal` | 2.43x | 0.0118 | 3.93 |
| `aalto_lin_std` | 2.43x | 0.00865 | 3.23 |
| `pilots` | 2.43x | 0.0109 | 3.49 |
| `ch14_m14_3` | 2.39x | 0.0604 | 5.99 |
| `sw_spline_s` | 2.39x | 0.0126 | 4.35 |
| `s2_nlf` | 2.37x | 0.0128 | 4.35 |
| `sw_skewnormal` | 2.36x | 0.0141 | 3.93 |
| `ch12_m12_5` | 2.36x | 5.2 | 15.6 |
| `i320_mi_nhanes` | 2.36x | 0.0178 | 4.44 |
| `kidscore_interaction_z` | 2.33x | 0.0209 | 3.29 |
| `aalto_lin` | 2.33x | 0.00812 | 3.05 |
| `kidscore_interaction_c2` | 2.33x | 0.0204 | 3.17 |
| `sw_re_bern` | 2.33x | 0.0146 | 4.66 |
| `i319_pois_re2` | 2.32x | 0.0388 | 4.85 |
| `s2_cumulative_cloglog` | 2.31x | 0.0159 | 3.77 |
| `accel_gp` | 2.31x | 0.0376 | 6.65 |
| `sw_vonmises` | 2.29x | 0.012 | 3.66 |
| `s2_me2` | 2.29x | 0.0209 | 6.69 |
| `Mh_model` | 2.28x | 0.0946 | 3.47 |
| `s2_mv_shared_re` | 2.28x | 0.0251 | 7.18 |
| `s2_sar_error` | 2.27x | 0.0291 | 4.36 |
| `ch13_m13_4b` | 2.26x | 0.044 | 3.39 |
| `kidscore_interaction` | 2.24x | 0.021 | 3.23 |
| `s2_sar` | 2.24x | 0.0286 | 4.26 |
| `kidscore_mom_work` | 2.23x | 0.022 | 3.13 |
| `kidscore_interaction_c` | 2.23x | 0.0213 | 3.2 |
| `logearn_height_male` | 2.23x | 0.0349 | 3.1 |
| `normal_mixture` | 2.22x | 0.224 | 3.29 |
| `logearn_logheight_male` | 2.22x | 0.0348 | 3.13 |
| `losscurve_sislob` | 2.22x | 0.0179 | 4.72 |
| `ch13_m13_6` | 2.22x | 0.0482 | 3.46 |
| `sw_nonlinear` | 2.22x | 0.0118 | 3.9 |
| `s2_gev` | 2.21x | 0.0206 | 3.85 |
| `ch11_m11_5` | 2.2x | 0.0463 | 3.17 |
| `logearn_interaction_z` | 2.2x | 0.0465 | 3.37 |
| `ch09_m9_5` | 2.2x | 0.00695 | 2.56 |
| `ch13_m13_4` | 2.2x | 0.0475 | 3.48 |
| `dogs_hierarchical` | 2.19x | 0.102 | 2.96 |
| `sw_re_slope` | 2.19x | 0.0183 | 6.83 |
| `sw_lognormal` | 2.19x | 0.0104 | 3.64 |
| `ch15_m15_6` | 2.18x | 0.0089 | 3.19 |
| `kidscore_momhsiq` | 2.18x | 0.0171 | 3.11 |
| `ch09_m9_3` | 2.18x | 0.00665 | 2.52 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.18x | 0.238 | 3.62 |
| `extra_hurdle_poisson` | 2.17x | 0.00779 | 2.59 |
| `ch13_m13_4nc` | 2.16x | 0.0544 | 3.43 |
| `ch14_m14_5` | 2.16x | 0.0189 | 3.05 |
| `dugongs_model` | 2.15x | 0.0101 | 3.03 |
| `logearn_interaction` | 2.15x | 0.0479 | 3.23 |
| `s2_mi_lognormal` | 2.15x | 0.0143 | 4.57 |
| `ch09_m9_4` | 2.13x | 0.0068 | 2.59 |
| `GLMM_Poisson_model` | 2.12x | 0.0112 | 3.85 |
| `low_dim_gauss_mix` | 2.1x | 0.26 | 3.74 |
| `accel_splines` | 2.1x | 0.0314 | 4.61 |
| `s2_s_by` | 2.09x | 0.0271 | 5.75 |
| `ch09_m9_2` | 2.07x | 0.00692 | 2.54 |
| `ch14_m14_4x` | 2.06x | 0.0154 | 2.88 |
| `seeds_centered_model` | 2.05x | 0.0121 | 3.92 |
| `sw_re_pois` | 2.05x | 0.0146 | 4.52 |
| `ldaK2` | 2.04x | 0.271 | 4.18 |
| `sw_se` | 2.03x | 0.00983 | 3.44 |
| `sw_dist_sigma` | 2.03x | 0.0142 | 3.8 |
| `logearn_height` | 2.03x | 0.026 | 2.92 |
| `log10earn_height` | 2.02x | 0.0259 | 2.92 |
| `sesame_one_pred_a` | 2.02x | 0.0116 | 2.84 |
| `ch11_m11_4` | 2.01x | 0.0424 | 3.08 |
| `s2_nl_noloop` | 2.01x | 0.011 | 3.83 |
| `sw_student` | 2x | 0.0115 | 3.8 |
| `radon_pooled` | 2x | 0.217 | 3.26 |
| `aalto_lin_std_t` | 2x | 0.00973 | 3.46 |
| `sw_spline_t2` | 2x | 0.0178 | 4.82 |
| `ch11_m11_10` | 1.98x | 0.00861 | 3.23 |
| `earn_height` | 1.98x | 0.0257 | 2.88 |
| `election88_full` | 1.98x | 1.19 | 7.42 |
| `sw_zi_poisson` | 1.98x | 0.0177 | 3.4 |
| `ch11_m11_6` | 1.98x | 0.00932 | 3 |
| `kidscore_momhs` | 1.97x | 0.0139 | 2.93 |
| `ch13_m13_5` | 1.97x | 0.0434 | 3.31 |
| `ch14_m14_4` | 1.97x | 0.015 | 2.88 |
| `GLM_Poisson_model` | 1.96x | 0.00931 | 3.3 |
| `ch13_m13_1` | 1.95x | 0.011 | 2.99 |
| `ch16_m16_4` | 1.93x | 0.0155 | 3.36 |
| `kidscore_momiq` | 1.93x | 0.0144 | 2.91 |
| `s2_cens_interval` | 1.91x | 0.0184 | 3.75 |
| `multi_occupancy` | 1.89x | 0.176 | 6.73 |
| `normal_mixture_k` | 1.88x | 1.04 | 5.33 |
| `ch13_m13_3` | 1.87x | 0.0115 | 3.02 |
| `ch11_m11_8` | 1.87x | 0.00855 | 2.92 |
| `ch13_m13_7nc` | 1.86x | 0.00623 | 2.34 |
| `ch13_m13_2` | 1.84x | 0.0117 | 3.12 |
| `s2_t2_by` | 1.83x | 0.0249 | 5.69 |
| `s2_index_mi` | 1.83x | 0.0169 | 4.18 |
| `ch09_mp` | 1.83x | 0.00691 | 2.32 |
| `soil_incubation` | 1.83x | 0.17 | 4.2 |
| `ch13_m13_7` | 1.81x | 0.0066 | 2.33 |
| `lotka_volterra` | 1.81x | 0.129 | 4.68 |
| `i319_pois_re` | 1.8x | 0.0293 | 4.54 |
| `sw_hurdle_gamma` | 1.78x | 0.0236 | 3.55 |
| `sw_asymlaplace` | 1.77x | 0.0209 | 3.65 |
| `cm_loggamma` | 1.77x | 0.0823 | 4.72 |
| `ch15_m15_3` | 1.74x | 0.0715 | 2.77 |
| `sw_mi` | 1.72x | 0.0152 | 3.84 |
| `hier_2pl` | 1.72x | 0.954 | 8.38 |
| `sw_zi_binomial` | 1.71x | 0.0232 | 3.44 |
| `cm_logweibull` | 1.7x | 0.0679 | 4.14 |
| `ch11_m11_11` | 1.69x | 0.00979 | 3.55 |
| `bym2_offset_only` | 1.69x | 0.199 | 4.69 |
| `cm_lognormal` | 1.69x | 0.126 | 5.61 |
| `cm_bisa` | 1.68x | 0.0699 | 4.22 |
| `2pl_latent_reg_irt` | 1.67x | 0.346 | 6.09 |
| `sw_ar` | 1.67x | 0.0184 | 4.26 |
| `seeds_stanified_model` | 1.66x | 0.0116 | 3.66 |
| `ch15_m15_4` | 1.66x | 0.0614 | 2.81 |
| `s2_threading` | 1.64x | 0.0112 | 3.38 |
| `cm_invweibull` | 1.63x | 0.076 | 4.13 |
| `cm_weibull` | 1.63x | 0.0741 | 4.12 |
| `cm_betagate` | 1.63x | 0.188 | 4.49 |
| `ch11_m11_7` | 1.61x | 0.00746 | 2.85 |
| `s2_mv_subset` | 1.6x | 0.014 | 3.49 |
| `cm_choco` | 1.6x | 0.242 | 5.36 |
| `sw_ma` | 1.6x | 0.019 | 4.24 |
| `sw_arma` | 1.6x | 0.0229 | 4.35 |
| `cm_lba1` | 1.59x | 0.121 | 5.06 |
| `cm_logstudent` | 1.59x | 0.0785 | 4.28 |
| `sw_zi_negbin` | 1.58x | 0.0269 | 3.49 |
| `cm_invgamma` | 1.58x | 0.0787 | 4.15 |
| `surgical_model` | 1.56x | 0.00977 | 3.43 |
| `seeds_model` | 1.56x | 0.0117 | 3.74 |
| `cm_gamma` | 1.55x | 0.0751 | 4.17 |
| `covid19imperial_v3` | 1.54x | 2.03 | 8.73 |
| `sw_gamma` | 1.52x | 0.0114 | 3.76 |
| `prophet` | 1.51x | 0.204 | 5.85 |
| `sw_gaussian` | 1.51x | 0.00983 | 3.31 |
| `s2_categorical_re` | 1.51x | 0.0408 | 5.47 |
| `s2_unstr` | 1.51x | 0.0492 | 8.72 |
| `cm_lnr_bench` | 1.5x | 0.959 | 9.57 |
| `Mb_model` | 1.5x | 0.218 | 3.54 |
| `sw_cratio_cs` | 1.5x | 0.469 | 5.69 |
| `sw_re_negbin` | 1.49x | 0.0203 | 4.77 |
| `sw_mv_norescor` | 1.47x | 0.0138 | 3.5 |
| `lsat_model` | 1.47x | 0.227 | 4.05 |
| `s2_zi_asymlaplace` | 1.47x | 0.0275 | 3.68 |
| `gp_regr` | 1.46x | 0.0191 | 5.47 |
| `aalto_binom` | 1.45x | 0.00609 | 2.37 |
| `gp_pois_regr` | 1.44x | 0.0177 | 5.65 |
| `covid19imperial_v2` | 1.44x | 2.05 | 8.64 |
| `aalto_bern` | 1.43x | 0.00659 | 2.56 |
| `Rate_4_model` | 1.43x | 0.00717 | 2.51 |
| `i320_sratio_cs` | 1.42x | 0.475 | 5.96 |
| `s2_rate` | 1.41x | 0.0122 | 3.6 |
| `Rate_1_model` | 1.41x | 0.00637 | 2.37 |
| `ch14_m14_2` | 1.4x | 0.096 | 7.18 |
| `s2_zoi_beta` | 1.39x | 0.0344 | 3.78 |
| `cm_exwald` | 1.39x | 0.17 | 6.06 |
| `hmm_example` | 1.39x | 0.116 | 4.22 |
| `ch11_m11_9` | 1.39x | 0.00732 | 2.6 |
| `s2_zi_beta` | 1.39x | 0.0333 | 3.74 |
| `cm_lba2` | 1.38x | 0.157 | 7.54 |
| `GLM_Binomial_model` | 1.38x | 0.0116 | 3.28 |
| `s2_mixture_theta` | 1.38x | 0.0454 | 4.49 |
| `sw_cumulative_cs` | 1.38x | 0.369 | 5.43 |
| `sw_exgaussian` | 1.38x | 0.0135 | 3.87 |
| `i320_gp_expquad` | 1.38x | 0.0644 | 7.35 |
| `aalto_binom2` | 1.37x | 0.00673 | 2.5 |
| `aalto_binomb` | 1.37x | 0.00642 | 2.42 |
| `s2_cumulative_probit` | 1.36x | 0.0304 | 3.71 |
| `i319_negbin_re` | 1.36x | 0.0584 | 4.83 |
| `ch15_m15_7` | 1.35x | 0.0553 | 7.65 |
| `irt_2pl` | 1.34x | 0.095 | 4.34 |
| `ch12_m12_1` | 1.34x | 0.0126 | 3.2 |
| `Rate_2_model` | 1.34x | 0.00758 | 2.55 |
| `blr` | 1.31x | 0.00859 | 3.07 |
| `ch14_m14_8nc` | 1.31x | 0.0249 | 6.01 |
| `ch11_m_pois` | 1.3x | 0.00673 | 2.53 |
| `ch12_m12_2` | 1.3x | 0.0122 | 3.97 |
| `Rate_5_model` | 1.3x | 0.00714 | 2.49 |
| `Rate_3_model` | 1.29x | 0.00724 | 2.4 |
| `wells_dist` | 1.29x | 0.13 | 3.17 |
| `sw_gp` | 1.28x | 0.107 | 7.39 |
| `ch12_m12_6` | 1.28x | 9.88 | 16.8 |
| `ch14_m14_6` | 1.28x | 0.439 | 7.29 |
| `ch14_m14_6x` | 1.27x | 0.445 | 7.26 |
| `nn_rbm1bJ10` | 1.27x | 0.805 | 6.45 |
| `ch14_m14_1` | 1.26x | 0.0353 | 6.92 |
| `sw_beta` | 1.26x | 0.0183 | 3.9 |
| `ch14_m14_10` | 1.23x | 1.85 | 6.51 |
| `sw_mixture` | 1.23x | 0.0403 | 4.76 |
| `sw_categorical` | 1.23x | 0.0151 | 4.38 |
| `ch14_m14_9` | 1.23x | 1.71 | 6.32 |
| `sw_mv_rescor` | 1.22x | 0.0419 | 6.13 |
| `sw_bernoulli` | 1.22x | 0.00998 | 3.27 |
| `ch14_m14_8` | 1.21x | 0.0241 | 4.78 |
| `sw_weibull` | 1.18x | 0.018 | 3.93 |
| `s2_frechet` | 1.18x | 0.019 | 3.95 |
| `s2_custom_vint` | 1.17x | 0.0356 | 3.55 |
| `ch14_m14_7` | 1.17x | 0.158 | 8.96 |
| `sw_binomial` | 1.17x | 0.0173 | 3.5 |
| `cm_rdm` | 1.17x | 0.315 | 10.9 |
| `logistic_regression_rhs` | 1.15x | 0.243 | 5.3 |
| `Survey_model` | 1.13x | 0.298 | 3.28 |
| `sw_poisson` | 1.13x | 0.0112 | 3.11 |
| `cm_lnr` | 1.11x | 0.39 | 14.1 |
| `sw_negbinomial` | 1.11x | 0.0163 | 3.42 |
| `sw_sratio` | 1.11x | 0.31 | 4.24 |
| `i320_sratio_plain` | 1.1x | 0.314 | 4.37 |
| `s2_multinomial` | 1.1x | 0.0574 | 3.98 |
| `ch14_m14_11` | 1.09x | 3.13 | 7.78 |
| `sw_cratio` | 1.09x | 0.318 | 4.23 |
| `s2_beta_binomial` | 1.08x | 0.0316 | 3.72 |
| `nn_rbm1bJ100` | 1.08x | 2.16e+03 | 2.32e+03 |
| `sw_mono` | 1.07x | 0.0202 | 4.13 |
| `s2_hurdle_negbin` | 1.06x | 0.0461 | 3.53 |
| `sw_trunc` | 1.05x | 0.0406 | 3.56 |
| `hmm_gaussian` | 1.05x | 1.48 | 6.22 |
| `s2_discrete_weibull` | 1.05x | 0.0267 | 3.52 |
| `s2_weights_trunc` | 1.04x | 0.0427 | 3.61 |
| `wells_dae_inter_model` | 1.04x | 0.113 | 3.46 |
| `aalto_poisson_simple` | 1.04x | 0.0122 | 2.67 |
| `i319_negbin_fixed` | 1.04x | 0.0496 | 3.44 |
| `wells_dist100_model` | 1.04x | 0.0935 | 3.29 |
| `wells_dae_model` | 1.03x | 0.111 | 3.34 |
| `bones_model` | 1.03x | 0.284 | 3.76 |
| `wells_dae_c_model` | 1.02x | 0.103 | 3.47 |
| `diamonds` | 1.02x | 0.187 | 3.59 |
| `hmm_drive_1` | 1.02x | 0.813 | 5.55 |
| `sw_cumulative` | 1.01x | 0.115 | 3.85 |
| `i319_pois_fixed` | 1.01x | 0.0229 | 3.14 |
| `s2_dirichlet` | 1.01x | 0.0723 | 4.42 |
| `one_comp_mm_elim_abs` | 1.01x | 2.48 | 6.3 |
| `s2_wiener` | 1x | 0.155 | 3.83 |
| `wells_interaction_model` | 0.995x | 0.111 | 3.36 |
| `hmm_drive_0` | 0.989x | 0.78 | 5.53 |
| `s2_ar_cov` | 0.988x | 0.0505 | 7.63 |
| `nes_logit_model` | 0.987x | 0.0403 | 3.19 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.987x | 0.11 | 3.42 |
| `wells_daae_c_model` | 0.985x | 0.114 | 3.48 |
| `wells_dist100ars_model` | 0.984x | 0.103 | 3.33 |
| `i320_pois_trunc_both` | 0.983x | 0.173 | 3.68 |
| `i320_pois_trunc_ub` | 0.979x | 0.138 | 3.58 |
| `s2_mo_simo_prior` | 0.977x | 0.0204 | 4.11 |
| `iohmm_reg` | 0.973x | 1.88 | 7.79 |
| `cm_betadiscrete` | 0.97x | 9.86 | 13.8 |
| `s2_cosy` | 0.957x | 0.0477 | 6.73 |
| `s2_fcor` | 0.942x | 0.0797 | 4.93 |
| `cm_ddm` | 0.923x | 96.6 | 98.4 |
| `i320_gp_matern32` | 0.905x | 0.166 | 7.31 |
| `cm_invgaussian` | 0.835x | 0.446 | 34 |
| `garch11` | 0.833x | 0.0507 | 2.97 |
| `aalto_gpareto` | 0.787x | 0.015 | 3.06 |
| `arma11` | 0.68x | 0.0361 | 2.99 |
| `s2_com_poisson` | 0.21x | 1.21 | 4.91 |

Total sampling is an estimate: each engine's measured setup time + 5,000 × its median warm gradient time. Full sampling is not run.

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
| Displayed workload | Setup + 5,000 × median warm gradient time |
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
