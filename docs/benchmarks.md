# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling it reuses a prepared autodiff graph and batches
independent work. Both shorten the first fit and every later gradient, by an
amount that depends on the model.

The current comparison covers **<!--gen:benchmark_models-->343<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->337<!--/gen--> accepted gradient
comparisons, the median model's gradient is **<!--gen:corpus_median-->2.11x<!--/gen--> faster**
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
in-process Python/R first-fit time.

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
Run `bec65da115c4bf4e` (2026-10-15): 343 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 33.3x | 0.157 | 7.76 |
| `gpcm_latent_reg_irt` | 12.7x | 0.338 | 8.29 |
| `grsm_latent_reg_irt` | 12.2x | 0.176 | 6.8 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11x | 0.0904 | 3.62 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.8x | 0.093 | 3.8 |
| `radon_county_intercept` | 10.2x | 0.142 | 3.89 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10.1x | 0.142 | 4.23 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.99x | 0.148 | 4.01 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.89x | 0.146 | 4.27 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 9.89x | 0.146 | 3.96 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.53x | 0.214 | 4.53 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.35x | 0.163 | 4.15 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.07x | 0.22 | 4.64 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 8.93x | 0.165 | 4.46 |
| `radon_pooled` | 8.2x | 0.094 | 3.41 |
| `aalto_poisson_hurdle` | 8x | 0.0438 | 3.67 |
| `dogs` | 7.91x | 0.0423 | 3.76 |
| `s2_custom_vreal` | 7.88x | 0.011 | 3.54 |
| `rats_model` | 7.54x | 0.00984 | 3.1 |
| `arK` | 7.22x | 0.0151 | 3.42 |
| `ch12_m12_3_alt` | 7x | 0.0141 | 2.61 |
| `ch12_m12_3` | 6.74x | 0.0149 | 2.56 |
| `ch16_m16_1` | 6.61x | 0.0206 | 3.06 |
| `M0_model` | 6.54x | 0.0143 | 2.74 |
| `Mt_model` | 5.64x | 0.0173 | 3.51 |
| `sw_nonlinear` | 5.24x | 0.0109 | 3.94 |
| `ch09_m5_8s` | 4.79x | 0.0107 | 2.98 |
| `ch14_m14_5` | 4.69x | 0.0119 | 2.88 |
| `ch09_m5_8s2` | 4.66x | 0.00867 | 2.91 |
| `s2_cumulative_cauchit` | 4.48x | 0.0145 | 3.7 |
| `mesquite` | 4.37x | 0.00995 | 3.13 |
| `logmesquite_logvas` | 4.36x | 0.00965 | 3.43 |
| `ch15_m15_1` | 4.33x | 0.00914 | 3.22 |
| `logmesquite` | 4.24x | 0.0094 | 3.17 |
| `logmesquite_logva` | 4.16x | 0.00815 | 3.19 |
| `logmesquite_logvash` | 4.15x | 0.00913 | 3.35 |
| `s2_nl_noloop` | 4.01x | 0.00966 | 3.96 |
| `election88_full` | 4.01x | 0.492 | 6.07 |
| `ch15_m15_2` | 3.99x | 0.00997 | 3.43 |
| `s2_car_icar` | 3.96x | 0.0128 | 4.39 |
| `sw_me` | 3.96x | 0.0111 | 4.15 |
| `kilpisjarvi` | 3.88x | 0.00794 | 2.83 |
| `nes` | 3.85x | 0.0491 | 3.37 |
| `Mth_model` | 3.85x | 0.0697 | 4.51 |
| `ch14_m14_4x` | 3.82x | 0.0105 | 2.86 |
| `s2_me2_nomecor` | 3.78x | 0.0138 | 4.44 |
| `logmesquite_logvolume` | 3.73x | 0.0079 | 3.01 |
| `sw_dist_sigma` | 3.72x | 0.012 | 3.83 |
| `ch14_m14_4` | 3.66x | 0.0109 | 2.83 |
| `sw_spline_s` | 3.66x | 0.011 | 4.51 |
| `kidscore_interaction_c` | 3.66x | 0.0132 | 3.11 |
| `s2_dist_sigma_re` | 3.65x | 0.013 | 4.95 |
| `kidscore_interaction_c2` | 3.64x | 0.0124 | 3.02 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.63x | 0.0225 | 4.96 |
| `s2_nlf` | 3.59x | 0.0112 | 4.02 |
| `GLMM1_model` | 3.57x | 0.0305 | 3.33 |
| `ch09_m9_1` | 3.55x | 0.00868 | 3.09 |
| `sw_weights` | 3.54x | 0.0111 | 3.52 |
| `sw_se` | 3.53x | 0.00986 | 3.56 |
| `s2_s_cc` | 3.53x | 0.0105 | 4.32 |
| `kidscore_interaction_z` | 3.5x | 0.0127 | 3.12 |
| `s2_mm` | 3.5x | 0.014 | 4.7 |
| `logearn_height_male` | 3.48x | 0.0189 | 3.21 |
| `ch09_m9_1_chains4` | 3.48x | 0.0087 | 3.11 |
| `s2_mm_weights` | 3.44x | 0.0143 | 4.7 |
| `kidscore_interaction` | 3.43x | 0.0128 | 3.07 |
| `kidscore_mom_work` | 3.43x | 0.014 | 2.97 |
| `logearn_interaction_z` | 3.4x | 0.0234 | 3.19 |
| `s2_car` | 3.39x | 0.0136 | 4.4 |
| `ch16_m16_4` | 3.37x | 0.0104 | 2.94 |
| `s2_cox_cens` | 3.35x | 0.0155 | 5.12 |
| `s2_s_by` | 3.33x | 0.024 | 5.96 |
| `logearn_interaction` | 3.32x | 0.024 | 3.16 |
| `s2_gp_by_approx` | 3.32x | 0.0194 | 6.03 |
| `aalto_grp_aov` | 3.31x | 0.00711 | 3.15 |
| `s2_gp_approx` | 3.3x | 0.014 | 5.24 |
| `s2_shifted_lognormal` | 3.3x | 0.0104 | 3.92 |
| `ch15_m15_9` | 3.29x | 0.0152 | 3.04 |
| `ch15_m15_8` | 3.26x | 0.0131 | 2.92 |
| `ch15_m15_5` | 3.25x | 0.00982 | 3.25 |
| `logearn_logheight_male` | 3.23x | 0.0191 | 2.98 |
| `kidscore_momhsiq` | 3.21x | 0.0116 | 2.96 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.21x | 0.00728 | 3.28 |
| `sw_lognormal` | 3.2x | 0.0107 | 3.74 |
| `radon_county` | 3.19x | 0.0651 | 3.3 |
| `s2_t2_by` | 3.17x | 0.0227 | 5.9 |
| `i319_gauss_re` | 3.17x | 0.0155 | 4.58 |
| `s2_car_esicar` | 3.16x | 0.0136 | 4.49 |
| `sw_spline_t2` | 3.11x | 0.0169 | 5.08 |
| `s2_gr_by` | 3.1x | 0.0135 | 4.96 |
| `sw_re_gauss` | 3.08x | 0.0121 | 4.56 |
| `sw_acat` | 3.04x | 0.112 | 4.2 |
| `s2_cox` | 2.92x | 0.0133 | 4.18 |
| `log10earn_height` | 2.91x | 0.015 | 2.85 |
| `logearn_height` | 2.91x | 0.0143 | 2.88 |
| `GLMM_Poisson_model` | 2.9x | 0.00904 | 3.84 |
| `s2_cumulative_cloglog` | 2.88x | 0.0133 | 3.7 |
| `s2_hurdle_cumulative` | 2.88x | 0.0177 | 4.17 |
| `sw_student` | 2.88x | 0.0108 | 3.86 |
| `pilots` | 2.87x | 0.009 | 3.39 |
| `bym2_offset_only` | 2.85x | 0.0917 | 5.42 |
| `sw_hurdle_lognormal` | 2.84x | 0.0159 | 3.95 |
| `ch15_m15_6` | 2.83x | 0.00791 | 2.99 |
| `earn_height` | 2.83x | 0.0145 | 2.85 |
| `sesame_one_pred_a` | 2.81x | 0.00866 | 2.83 |
| `blr` | 2.79x | 0.0101 | 3.72 |
| `kidscore_momiq` | 2.77x | 0.0105 | 2.9 |
| `ch12_m12_7` | 2.75x | 2 | 8.74 |
| `s2_mi_lognormal` | 2.74x | 0.0132 | 4.62 |
| `Mtbh_model` | 2.74x | 0.0588 | 5.11 |
| `aalto_grp_prior_mean` | 2.74x | 0.00715 | 3.19 |
| `dogs_nonhierarchical` | 2.73x | 0.0588 | 7.02 |
| `s2_gr_student` | 2.7x | 0.0132 | 5.19 |
| `ch12_m12_5` | 2.69x | 2.08 | 8.96 |
| `GLM_Poisson_model` | 2.68x | 0.00788 | 3.31 |
| `kidscore_momhs` | 2.68x | 0.00978 | 2.94 |
| `aalto_lin` | 2.68x | 0.0102 | 3.74 |
| `accel_gp` | 2.68x | 0.03 | 6.46 |
| `aalto_lin_std` | 2.67x | 0.0101 | 3.97 |
| `ch13_m13_4nc` | 2.64x | 0.0291 | 3.35 |
| `cm_geg` | 2.63x | 0.0634 | 4 |
| `sw_gamma` | 2.63x | 0.0114 | 3.86 |
| `s2_mmc` | 2.61x | 0.019 | 7.04 |
| `eight_schools_centered` | 2.59x | 0.00735 | 3 |
| `hierarchical_gp` | 2.57x | 0.0625 | 9.18 |
| `aalto_grp_prior_mean_var` | 2.56x | 0.00806 | 4.56 |
| `sw_hurdle_pois` | 2.55x | 0.0175 | 3.72 |
| `logistic_regression_rhs` | 2.55x | 0.105 | 5.13 |
| `cm_exgaussian` | 2.52x | 0.0438 | 3.75 |
| `ldaK5` | 2.5x | 5.15 | 15.3 |
| `s2_sar_error` | 2.5x | 0.0178 | 4.28 |
| `accel_splines` | 2.5x | 0.0235 | 5.35 |
| `sw_skewnormal` | 2.5x | 0.0117 | 4.04 |
| `sw_acat_cs` | 2.48x | 0.161 | 5.32 |
| `s2_gev` | 2.47x | 0.0157 | 3.82 |
| `losscurve_sislob` | 2.45x | 0.0154 | 4.33 |
| `s2_sar` | 2.45x | 0.0178 | 4.34 |
| `s2_rate` | 2.44x | 0.0102 | 3.81 |
| `ch15_m15_3` | 2.43x | 0.0357 | 2.71 |
| `Mh_model` | 2.43x | 0.0472 | 3.39 |
| `i319_pois_re2` | 2.39x | 0.0245 | 4.87 |
| `sw_re_bern` | 2.36x | 0.0124 | 4.69 |
| `s2_me2` | 2.36x | 0.0168 | 6.75 |
| `ch14_m14_3` | 2.36x | 0.0352 | 5.95 |
| `seeds_centered_model` | 2.32x | 0.0093 | 3.88 |
| `ch15_m15_4` | 2.31x | 0.0304 | 2.79 |
| `ch11_m11_10` | 2.31x | 0.00836 | 3.23 |
| `low_dim_gauss_mix` | 2.26x | 0.104 | 3.43 |
| `dogs_hierarchical` | 2.26x | 0.0563 | 2.88 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.25x | 0.1 | 3.41 |
| `extra_hurdle_poisson` | 2.24x | 0.0069 | 2.78 |
| `lsat_model` | 2.23x | 0.0969 | 3.95 |
| `hier_2pl` | 2.21x | 0.4 | 7.68 |
| `i320_mi_nhanes` | 2.21x | 0.0176 | 4.51 |
| `s2_mv_shared_re` | 2.21x | 0.021 | 7.1 |
| `ch13_m13_4b` | 2.2x | 0.0239 | 3.38 |
| `ch13_m13_4` | 2.19x | 0.0287 | 3.46 |
| `normal_mixture` | 2.19x | 0.0957 | 2.83 |
| `ch11_m11_5` | 2.18x | 0.0286 | 3.48 |
| `ch09_m9_2` | 2.18x | 0.00695 | 2.54 |
| `ch09_m9_4` | 2.16x | 0.00709 | 2.59 |
| `sw_vonmises` | 2.15x | 0.0108 | 3.72 |
| `ch09_m9_3` | 2.15x | 0.00637 | 2.53 |
| `ch09_m9_5` | 2.14x | 0.00671 | 2.58 |
| `sw_zi_poisson` | 2.13x | 0.0156 | 3.5 |
| `ch13_m13_6` | 2.13x | 0.0293 | 3.45 |
| `aalto_lin_std_t` | 2.13x | 0.0109 | 4.26 |
| `sw_re_pois` | 2.12x | 0.0128 | 4.79 |
| `ldaK2` | 2.11x | 0.121 | 3.95 |
| `irt_2pl` | 2.09x | 0.0436 | 4.25 |
| `2pl_latent_reg_irt` | 2.09x | 0.177 | 6.31 |
| `dugongs_model` | 2.05x | 0.00883 | 3.06 |
| `seeds_model` | 2.05x | 0.0094 | 3.71 |
| `multi_occupancy` | 2.03x | 0.0827 | 6.16 |
| `sw_re_slope` | 1.99x | 0.0155 | 6.82 |
| `ch13_m13_5` | 1.98x | 0.0253 | 3.28 |
| `ch11_m11_4` | 1.98x | 0.0258 | 3.31 |
| `ch11_m11_6` | 1.96x | 0.00915 | 3.36 |
| `normal_mixture_k` | 1.96x | 0.44 | 4.2 |
| `prophet` | 1.95x | 0.0945 | 5.09 |
| `sw_exgaussian` | 1.95x | 0.0118 | 3.92 |
| `s2_index_mi` | 1.94x | 0.0134 | 4.22 |
| `ch13_m13_7` | 1.94x | 0.00658 | 2.33 |
| `soil_incubation` | 1.89x | 0.0762 | 3.59 |
| `seeds_stanified_model` | 1.89x | 0.00982 | 3.6 |
| `ctsem_ctsm` | 1.87x | 2.79 | 92.1 |
| `ch11_m11_8` | 1.85x | 0.00741 | 2.97 |
| `ctsem_ctsm_400` | 1.85x | 22.9 | 129 |
| `ch13_m13_3` | 1.83x | 0.00904 | 3.03 |
| `sw_ma` | 1.82x | 0.0151 | 4.26 |
| `ch13_m13_1` | 1.82x | 0.00904 | 2.99 |
| `sw_mi` | 1.81x | 0.0131 | 3.9 |
| `ch13_m13_2` | 1.81x | 0.00909 | 3.16 |
| `s2_cens_interval` | 1.81x | 0.0142 | 3.8 |
| `i319_pois_re` | 1.8x | 0.0191 | 4.53 |
| `sw_hurdle_gamma` | 1.79x | 0.0179 | 3.91 |
| `ch11_m11_11` | 1.79x | 0.00837 | 3.87 |
| `cm_invweibull` | 1.77x | 0.038 | 4.12 |
| `ch13_m13_7nc` | 1.77x | 0.00595 | 2.42 |
| `cm_weibull` | 1.77x | 0.0378 | 4.09 |
| `sw_arma` | 1.77x | 0.0159 | 4.44 |
| `lotka_volterra` | 1.76x | 0.0585 | 4.44 |
| `ch09_mp` | 1.76x | 0.00641 | 2.32 |
| `cm_choco` | 1.75x | 0.112 | 4.67 |
| `sw_zi_binomial` | 1.75x | 0.0176 | 3.45 |
| `s2_threading` | 1.75x | 0.00947 | 3.38 |
| `sw_ar` | 1.75x | 0.0144 | 4.5 |
| `cm_bisa` | 1.74x | 0.0352 | 4.06 |
| `cm_invgamma` | 1.74x | 0.0385 | 4.16 |
| `cm_loggamma` | 1.73x | 0.0435 | 4.2 |
| `wells_dist` | 1.72x | 0.0556 | 3.06 |
| `s2_mv_subset` | 1.71x | 0.012 | 3.48 |
| `cm_logweibull` | 1.68x | 0.034 | 4.07 |
| `sw_asymlaplace` | 1.67x | 0.0152 | 3.58 |
| `sw_beta` | 1.67x | 0.0123 | 3.91 |
| `cm_gamma` | 1.66x | 0.0383 | 4.1 |
| `cm_logstudent` | 1.64x | 0.039 | 4.18 |
| `cm_lognormal` | 1.63x | 0.0734 | 4.27 |
| `GLM_Binomial_model` | 1.62x | 0.00937 | 3.3 |
| `cm_betagate` | 1.61x | 0.0827 | 4.09 |
| `sw_zi_negbin` | 1.61x | 0.0188 | 3.49 |
| `sw_mv_norescor` | 1.6x | 0.0119 | 3.55 |
| `cm_lnr_bench` | 1.6x | 0.545 | 5.26 |
| `sw_gaussian` | 1.6x | 0.00977 | 3.3 |
| `ch11_m11_7` | 1.58x | 0.00728 | 2.87 |
| `surgical_model` | 1.57x | 0.00871 | 3.47 |
| `s2_zi_beta` | 1.55x | 0.0226 | 4.05 |
| `s2_zoi_beta` | 1.55x | 0.0233 | 3.71 |
| `s2_categorical_re` | 1.51x | 0.0252 | 5.45 |
| `covid19imperial_v2` | 1.5x | 1.31 | 7.68 |
| `gp_regr` | 1.49x | 0.0119 | 5.49 |
| `s2_zi_asymlaplace` | 1.49x | 0.0195 | 3.86 |
| `cm_lba1` | 1.49x | 0.0616 | 4.43 |
| `sw_weibull` | 1.49x | 0.0123 | 3.93 |
| `s2_gp_by_gr` | 1.49x | 0.0422 | 9.12 |
| `sw_re_negbin` | 1.47x | 0.015 | 4.76 |
| `s2_frechet` | 1.47x | 0.0132 | 4.04 |
| `s2_unstr` | 1.46x | 0.0372 | 7.6 |
| `cm_lba2` | 1.45x | 0.0886 | 4.71 |
| `covid19imperial_v3` | 1.45x | 1.3 | 7.7 |
| `s2_mixture_theta` | 1.44x | 0.0268 | 4.63 |
| `aalto_binom` | 1.44x | 0.00671 | 2.39 |
| `Mb_model` | 1.44x | 0.113 | 3.45 |
| `Rate_1_model` | 1.43x | 0.00622 | 2.39 |
| `gp_pois_regr` | 1.43x | 0.0114 | 5.77 |
| `ch11_m11_9` | 1.42x | 0.00751 | 2.61 |
| `ch14_m14_2` | 1.41x | 0.0467 | 7.13 |
| `sw_mono` | 1.39x | 0.0158 | 4.07 |
| `ch12_m12_6` | 1.39x | 4.11 | 9.8 |
| `Rate_4_model` | 1.39x | 0.00633 | 2.53 |
| `cm_exwald` | 1.37x | 0.108 | 4.32 |
| `i320_gp_expquad` | 1.37x | 0.0326 | 7.05 |
| `hmm_example` | 1.36x | 0.0596 | 4.15 |
| `Rate_2_model` | 1.35x | 0.00677 | 2.57 |
| `i319_negbin_re` | 1.34x | 0.031 | 4.78 |
| `aalto_binomb` | 1.34x | 0.00588 | 2.44 |
| `aalto_bern` | 1.33x | 0.00655 | 2.57 |
| `Rate_3_model` | 1.32x | 0.0069 | 2.48 |
| `s2_mo_simo_prior` | 1.32x | 0.0166 | 4 |
| `Rate_5_model` | 1.31x | 0.00735 | 2.5 |
| `aalto_binom2` | 1.31x | 0.00646 | 2.52 |
| `ch12_m12_1` | 1.31x | 0.00919 | 3.21 |
| `ch11_m_pois` | 1.31x | 0.0069 | 2.53 |
| `sw_gp` | 1.3x | 0.0495 | 7.34 |
| `ch14_m14_6x` | 1.29x | 0.184 | 6.88 |
| `ch14_m14_1` | 1.29x | 0.0204 | 6.94 |
| `sw_binomial` | 1.29x | 0.0121 | 3.58 |
| `sw_mixture` | 1.29x | 0.0243 | 4.8 |
| `ch12_m12_2` | 1.28x | 0.0107 | 4 |
| `s2_beta_binomial` | 1.28x | 0.0182 | 3.73 |
| `nn_rbm1bJ10` | 1.25x | 0.331 | 5.88 |
| `ch14_m14_6` | 1.25x | 0.193 | 6.89 |
| `sw_cratio` | 1.23x | 0.142 | 3.8 |
| `i320_sratio_plain` | 1.23x | 0.14 | 3.93 |
| `ch14_m14_8nc` | 1.22x | 0.0166 | 5.73 |
| `cm_rdm` | 1.22x | 0.226 | 4.62 |
| `s2_custom_vint` | 1.22x | 0.0195 | 3.56 |
| `s2_cumulative_probit` | 1.21x | 0.0178 | 3.75 |
| `sw_sratio` | 1.19x | 0.139 | 3.88 |
| `sw_mv_rescor` | 1.18x | 0.0249 | 6.22 |
| `sw_bernoulli` | 1.17x | 0.00863 | 3.29 |
| `ch14_m14_10` | 1.17x | 0.825 | 5 |
| `s2_multinomial` | 1.16x | 0.0289 | 4.07 |
| `ch14_m14_9` | 1.16x | 0.762 | 4.9 |
| `i320_sratio_cs` | 1.15x | 0.201 | 5.21 |
| `sw_poisson` | 1.15x | 0.0095 | 3.13 |
| `sw_cratio_cs` | 1.14x | 0.204 | 4.94 |
| `Survey_model` | 1.13x | 0.131 | 3.12 |
| `cm_lnr` | 1.12x | 0.308 | 4.68 |
| `ch14_m14_7` | 1.12x | 0.0744 | 8.86 |
| `sw_categorical` | 1.09x | 0.0128 | 4.36 |
| `s2_hurdle_negbin` | 1.09x | 0.0276 | 3.51 |
| `s2_discrete_weibull` | 1.08x | 0.0173 | 3.52 |
| `sw_negbinomial` | 1.07x | 0.0123 | 3.4 |
| `s2_logistic_normal` | 1.07x | 0.0543 | 6.45 |
| `ch14_m14_8` | 1.07x | 0.0165 | 4.51 |
| `nn_rbm1bJ100` | 1.06x | 870 | 923 |
| `sw_trunc` | 1.06x | 0.0225 | 3.61 |
| `wells_dist100ars_model` | 1.06x | 0.0453 | 3.26 |
| `s2_dirichlet` | 1.05x | 0.0362 | 4.42 |
| `arma11` | 1.05x | 0.022 | 3.56 |
| `ch15_m15_7` | 1.05x | 0.0336 | 7.25 |
| `iohmm_reg` | 1.04x | 0.868 | 6.73 |
| `s2_weights_trunc` | 1.04x | 0.0232 | 3.65 |
| `aalto_poisson_simple` | 1.04x | 0.0114 | 3.21 |
| `i319_pois_fixed` | 1.04x | 0.0137 | 3.15 |
| `nes_logit_model` | 1.03x | 0.0201 | 3.17 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.03x | 0.0495 | 3.37 |
| `wells_dist100_model` | 1.03x | 0.0425 | 3.24 |
| `wells_dae_c_model` | 1.02x | 0.0475 | 3.43 |
| `diamonds` | 1.02x | 0.0903 | 3.52 |
| `wells_daae_c_model` | 1.01x | 0.0504 | 3.42 |
| `s2_wiener` | 1.01x | 0.0733 | 4.2 |
| `bones_model` | 1.01x | 0.132 | 3.84 |
| `garch11` | 1.01x | 0.0266 | 3.14 |
| `i320_pois_trunc_ub` | 1x | 0.0611 | 3.56 |
| `wells_dae_inter_model` | 1x | 0.0522 | 3.39 |
| `hmm_gaussian` | 1x | 0.713 | 5.4 |
| `cm_betadiscrete` | 1x | 3.98 | 8.17 |
| `hmm_drive_0` | 1x | 0.335 | 5.09 |
| `wells_interaction_model` | 0.993x | 0.0496 | 3.34 |
| `sw_cumulative` | 0.99x | 0.0541 | 3.92 |
| `one_comp_mm_elim_abs` | 0.988x | 0.989 | 4.41 |
| `cm_ddm` | 0.985x | 39.5 | 45.9 |
| `wells_dae_model` | 0.98x | 0.05 | 3.28 |
| `hmm_drive_1` | 0.977x | 0.362 | 5.11 |
| `s2_ar_cov` | 0.975x | 0.0338 | 7.25 |
| `i319_negbin_fixed` | 0.975x | 0.0254 | 3.41 |
| `s2_cosy` | 0.97x | 0.0315 | 6.48 |
| `sw_cumulative_cs` | 0.969x | 0.19 | 4.92 |
| `i320_pois_trunc_both` | 0.958x | 0.0752 | 3.61 |
| `s2_fcor` | 0.935x | 0.0378 | 4.94 |
| `i320_gp_matern32` | 0.91x | 0.0782 | 7.02 |
| `ch14_m14_11` | 0.884x | 1.68 | 5.45 |
| `aalto_gpareto` | 0.825x | 0.0136 | 2.93 |
| `cm_invgaussian` | 0.412x | 0.386 | 4.79 |
| `s2_com_poisson` | 0.258x | 0.52 | 3.77 |

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
Stanli timings are from the default run `bec65da115c4bf4e`. The CmdStan timings with loop vectorization are reused from run `8ab8978d93458c88` (2026-10-10). `s2_gp_by_gr` is from run `532c791f4c877dc4` (2026-10-16). `ctsem_ctsm_400` is from run `ad937274cf01193a` (2026-10-16). Each ratio is CmdStan's median gradient time divided by Stanli's.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 31.7x | 0.157 | 7.54 |
| `gpcm_latent_reg_irt` | 12.4x | 0.338 | 8.43 |
| `grsm_latent_reg_irt` | 12.2x | 0.176 | 7 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.3x | 0.0904 | 3.63 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11.3x | 0.093 | 3.8 |
| `radon_county_intercept` | 8.37x | 0.142 | 3.72 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.3x | 0.146 | 3.81 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.16x | 0.148 | 3.82 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8.06x | 0.142 | 4.01 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.66x | 0.146 | 3.97 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.57x | 0.163 | 3.94 |
| `s2_custom_vreal` | 7.44x | 0.011 | 3.44 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.43x | 0.214 | 4.22 |
| `aalto_poisson_hurdle` | 7.4x | 0.0438 | 3.07 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.22x | 0.22 | 4.39 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.2x | 0.165 | 4.24 |
| `ch12_m12_3_alt` | 7.1x | 0.0141 | 2.55 |
| `ch12_m12_3` | 6.92x | 0.0149 | 2.52 |
| `ch16_m16_1` | 6.53x | 0.0206 | 3.04 |
| `M0_model` | 6.52x | 0.0143 | 2.74 |
| `rats_model` | 6.37x | 0.00984 | 3.09 |
| `Mt_model` | 4.95x | 0.0173 | 3.5 |
| `arK` | 4.59x | 0.0151 | 2.88 |
| `Mth_model` | 4.06x | 0.0697 | 4.49 |
| `s2_cumulative_cauchit` | 3.95x | 0.0145 | 3.75 |
| `s2_car_icar` | 3.86x | 0.0128 | 4.38 |
| `dogs` | 3.57x | 0.0423 | 4.23 |
| `s2_mm_weights` | 3.52x | 0.0143 | 4.7 |
| `sw_weights` | 3.44x | 0.0111 | 3.46 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.38x | 0.0225 | 4.86 |
| `s2_car` | 3.37x | 0.0136 | 4.7 |
| `GLMM1_model` | 3.35x | 0.0305 | 3.27 |
| `sw_me` | 3.33x | 0.0111 | 4.27 |
| `cm_exgaussian` | 3.28x | 0.0438 | 4.42 |
| `s2_gp_by_approx` | 3.26x | 0.0194 | 6.5 |
| `s2_cox_cens` | 3.26x | 0.0155 | 4.98 |
| `s2_mm` | 3.24x | 0.014 | 4.71 |
| `s2_me2_nomecor` | 3.21x | 0.0138 | 4.53 |
| `s2_car_esicar` | 3.18x | 0.0136 | 4.79 |
| `i319_gauss_re` | 3.18x | 0.0155 | 4.58 |
| `s2_dist_sigma_re` | 3.15x | 0.013 | 4.96 |
| `logmesquite_logva` | 3.11x | 0.00815 | 3.33 |
| `mesquite` | 3.11x | 0.00995 | 3.38 |
| `logmesquite_logvash` | 3.09x | 0.00913 | 3.54 |
| `sw_re_gauss` | 3.06x | 0.0121 | 4.59 |
| `radon_county` | 2.99x | 0.0651 | 3.29 |
| `s2_gr_by` | 2.99x | 0.0135 | 4.93 |
| `ch09_m9_1_chains4` | 2.99x | 0.0087 | 3.32 |
| `logmesquite_logvas` | 2.98x | 0.00965 | 3.65 |
| `aalto_grp_aov` | 2.94x | 0.00711 | 3.05 |
| `s2_hurdle_cumulative` | 2.92x | 0.0177 | 4.17 |
| `logmesquite` | 2.91x | 0.0094 | 3.42 |
| `logmesquite_logvolume` | 2.91x | 0.0079 | 3.03 |
| `ch09_m9_1` | 2.89x | 0.00868 | 3.29 |
| `ch15_m15_1` | 2.89x | 0.00914 | 3.41 |
| `s2_cox` | 2.88x | 0.0133 | 4.11 |
| `cm_geg` | 2.85x | 0.0634 | 4.66 |
| `ch15_m15_5` | 2.76x | 0.00982 | 3.69 |
| `sw_acat` | 2.73x | 0.112 | 4.21 |
| `s2_gr_student` | 2.68x | 0.0132 | 5.29 |
| `dogs_nonhierarchical` | 2.68x | 0.0588 | 6.9 |
| `eight_schools_centered` | 2.66x | 0.00735 | 2.99 |
| `ch15_m15_9` | 2.64x | 0.0152 | 3.15 |
| `sw_acat_cs` | 2.63x | 0.161 | 5.43 |
| `hierarchical_gp` | 2.62x | 0.0625 | 9.16 |
| `ch15_m15_2` | 2.62x | 0.00997 | 3.64 |
| `Mtbh_model` | 2.58x | 0.0588 | 5 |
| `ch09_m5_8s` | 2.58x | 0.0107 | 3.03 |
| `ch15_m15_8` | 2.58x | 0.0131 | 2.9 |
| `sw_hurdle_lognormal` | 2.57x | 0.0159 | 3.55 |
| `ch12_m12_7` | 2.54x | 2 | 8.35 |
| `aalto_grp_prior_mean` | 2.54x | 0.00715 | 3.13 |
| `pilots` | 2.51x | 0.009 | 3.49 |
| `kilpisjarvi` | 2.5x | 0.00794 | 2.8 |
| `s2_mmc` | 2.5x | 0.019 | 7.02 |
| `ldaK5` | 2.47x | 5.15 | 15.3 |
| `s2_s_cc` | 2.46x | 0.0105 | 4.21 |
| `s2_gp_approx` | 2.45x | 0.014 | 5.51 |
| `s2_shifted_lognormal` | 2.45x | 0.0104 | 3.93 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.44x | 0.00728 | 3.21 |
| `nes` | 2.44x | 0.0491 | 3.7 |
| `ch12_m12_5` | 2.43x | 2.08 | 8.37 |
| `sw_hurdle_pois` | 2.42x | 0.0175 | 3.42 |
| `ch09_m5_8s2` | 2.41x | 0.00867 | 3.03 |
| `sw_skewnormal` | 2.41x | 0.0117 | 3.92 |
| `sw_re_bern` | 2.39x | 0.0124 | 4.66 |
| `s2_nlf` | 2.38x | 0.0112 | 4.35 |
| `kidscore_interaction_c2` | 2.38x | 0.0124 | 3.15 |
| `i319_pois_re2` | 2.37x | 0.0245 | 4.82 |
| `s2_cumulative_cloglog` | 2.37x | 0.0133 | 3.77 |
| `kidscore_interaction_z` | 2.37x | 0.0127 | 3.27 |
| `sw_spline_s` | 2.32x | 0.011 | 4.35 |
| `ch14_m14_3` | 2.31x | 0.0352 | 5.92 |
| `aalto_grp_prior_mean_var` | 2.29x | 0.00806 | 3.86 |
| `Mh_model` | 2.28x | 0.0472 | 3.36 |
| `logearn_height_male` | 2.26x | 0.0189 | 3.06 |
| `s2_sar` | 2.25x | 0.0178 | 4.24 |
| `aalto_lin_std` | 2.24x | 0.0101 | 3.23 |
| `sw_vonmises` | 2.24x | 0.0108 | 3.65 |
| `ch13_m13_4b` | 2.24x | 0.0239 | 3.34 |
| `i320_mi_nhanes` | 2.24x | 0.0176 | 4.43 |
| `low_dim_gauss_mix` | 2.24x | 0.104 | 3.43 |
| `s2_sar_error` | 2.24x | 0.0178 | 4.33 |
| `ch14_m14_5` | 2.24x | 0.0119 | 3.04 |
| `normal_mixture` | 2.23x | 0.0957 | 3.01 |
| `kidscore_interaction_c` | 2.23x | 0.0132 | 3.18 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.22x | 0.1 | 3.32 |
| `losscurve_sislob` | 2.21x | 0.0154 | 4.71 |
| `aalto_lin` | 2.2x | 0.0102 | 3.05 |
| `s2_me2` | 2.19x | 0.0168 | 6.68 |
| `ch13_m13_6` | 2.19x | 0.0293 | 3.41 |
| `logearn_interaction_z` | 2.19x | 0.0234 | 3.32 |
| `accel_gp` | 2.19x | 0.03 | 6.62 |
| `kidscore_mom_work` | 2.18x | 0.014 | 3.12 |
| `dogs_hierarchical` | 2.18x | 0.0563 | 2.86 |
| `kidscore_interaction` | 2.18x | 0.0128 | 3.21 |
| `s2_mv_shared_re` | 2.18x | 0.021 | 7.17 |
| `s2_gev` | 2.17x | 0.0157 | 3.84 |
| `sw_lognormal` | 2.17x | 0.0107 | 3.63 |
| `logearn_interaction` | 2.17x | 0.024 | 3.18 |
| `kidscore_momhsiq` | 2.16x | 0.0116 | 3.1 |
| `ch15_m15_6` | 2.16x | 0.00791 | 3.19 |
| `logearn_logheight_male` | 2.16x | 0.0191 | 3.09 |
| `GLMM_Poisson_model` | 2.15x | 0.00904 | 3.85 |
| `ch13_m13_4nc` | 2.14x | 0.0291 | 3.37 |
| `s2_mi_lognormal` | 2.13x | 0.0132 | 4.56 |
| `sw_nonlinear` | 2.12x | 0.0109 | 3.89 |
| `extra_hurdle_poisson` | 2.12x | 0.0069 | 2.59 |
| `sw_re_slope` | 2.12x | 0.0155 | 6.83 |
| `ch09_m9_5` | 2.12x | 0.00671 | 2.56 |
| `ch09_m9_4` | 2.12x | 0.00709 | 2.58 |
| `ch13_m13_4` | 2.11x | 0.0287 | 3.43 |
| `s2_s_by` | 2.09x | 0.024 | 5.74 |
| `ch11_m11_5` | 2.09x | 0.0286 | 3.13 |
| `ch09_m9_3` | 2.09x | 0.00637 | 2.52 |
| `dugongs_model` | 2.08x | 0.00883 | 3.03 |
| `log10earn_height` | 2.08x | 0.015 | 2.89 |
| `seeds_centered_model` | 2.07x | 0.0093 | 3.91 |
| `sesame_one_pred_a` | 2.07x | 0.00866 | 2.83 |
| `sw_re_pois` | 2.06x | 0.0128 | 4.51 |
| `logearn_height` | 2.06x | 0.0143 | 2.9 |
| `sw_se` | 2.05x | 0.00986 | 3.44 |
| `ctsem_ctsm` | 2.05x | 2.79 | 441 |
| `sw_zi_poisson` | 2.03x | 0.0156 | 3.39 |
| `ch14_m14_4x` | 2.02x | 0.0105 | 2.87 |
| `kidscore_momhs` | 2.02x | 0.00978 | 2.92 |
| `ch09_m9_2` | 2.01x | 0.00695 | 2.54 |
| `radon_pooled` | 1.99x | 0.094 | 3.02 |
| `earn_height` | 1.98x | 0.0145 | 2.86 |
| `sw_dist_sigma` | 1.98x | 0.012 | 3.79 |
| `ldaK2` | 1.97x | 0.121 | 3.88 |
| `ch13_m13_5` | 1.97x | 0.0253 | 3.28 |
| `ch14_m14_4` | 1.96x | 0.0109 | 2.87 |
| `election88_full` | 1.96x | 0.492 | 6.04 |
| `kidscore_momiq` | 1.96x | 0.0105 | 2.91 |
| `ctsem_ctsm_400` | 1.96x | 22.9 | 501 |
| `ch11_m11_10` | 1.95x | 0.00836 | 3.23 |
| `sw_student` | 1.93x | 0.0108 | 3.8 |
| `normal_mixture_k` | 1.92x | 0.44 | 4.2 |
| `GLM_Poisson_model` | 1.92x | 0.00788 | 3.3 |
| `s2_nl_noloop` | 1.92x | 0.00966 | 3.83 |
| `sw_spline_t2` | 1.91x | 0.0169 | 4.82 |
| `accel_splines` | 1.9x | 0.0235 | 4.59 |
| `ch11_m11_4` | 1.89x | 0.0258 | 3.05 |
| `ch16_m16_4` | 1.89x | 0.0104 | 3.35 |
| `multi_occupancy` | 1.88x | 0.0827 | 6.56 |
| `ch13_m13_7` | 1.88x | 0.00658 | 2.33 |
| `ch13_m13_1` | 1.88x | 0.00904 | 2.98 |
| `ch11_m11_6` | 1.87x | 0.00915 | 3 |
| `ch13_m13_3` | 1.86x | 0.00904 | 3.01 |
| `s2_cens_interval` | 1.86x | 0.0142 | 3.74 |
| `aalto_lin_std_t` | 1.83x | 0.0109 | 3.46 |
| `i319_pois_re` | 1.82x | 0.0191 | 4.52 |
| `ch11_m11_8` | 1.8x | 0.00741 | 2.92 |
| `ch15_m15_3` | 1.8x | 0.0357 | 2.7 |
| `s2_t2_by` | 1.79x | 0.0227 | 5.68 |
| `ch13_m13_7nc` | 1.78x | 0.00595 | 2.34 |
| `soil_incubation` | 1.78x | 0.0762 | 4.03 |
| `s2_index_mi` | 1.76x | 0.0134 | 4.17 |
| `lotka_volterra` | 1.76x | 0.0585 | 4.55 |
| `ch13_m13_2` | 1.76x | 0.00909 | 3.12 |
| `sw_mi` | 1.75x | 0.0131 | 3.83 |
| `sw_asymlaplace` | 1.75x | 0.0152 | 3.64 |
| `cm_loggamma` | 1.74x | 0.0435 | 4.65 |
| `cm_logweibull` | 1.73x | 0.034 | 4.09 |
| `cm_bisa` | 1.72x | 0.0352 | 4.16 |
| `ch09_mp` | 1.71x | 0.00641 | 2.32 |
| `ch11_m11_11` | 1.7x | 0.00837 | 3.55 |
| `s2_threading` | 1.69x | 0.00947 | 3.38 |
| `hier_2pl` | 1.68x | 0.4 | 7.41 |
| `ch15_m15_4` | 1.68x | 0.0304 | 2.76 |
| `cm_betagate` | 1.68x | 0.0827 | 4.32 |
| `sw_hurdle_gamma` | 1.67x | 0.0179 | 3.54 |
| `seeds_model` | 1.66x | 0.0094 | 3.73 |
| `sw_zi_binomial` | 1.65x | 0.0176 | 3.43 |
| `cm_lognormal` | 1.65x | 0.0734 | 5.52 |
| `cm_invweibull` | 1.64x | 0.038 | 4.07 |
| `cm_weibull` | 1.63x | 0.0378 | 4.06 |
| `2pl_latent_reg_irt` | 1.62x | 0.177 | 5.76 |
| `sw_ma` | 1.62x | 0.0151 | 4.23 |
| `seeds_stanified_model` | 1.62x | 0.00982 | 3.65 |
| `cm_logstudent` | 1.61x | 0.039 | 4.22 |
| `cm_invgamma` | 1.61x | 0.0385 | 4.09 |
| `sw_zi_negbin` | 1.61x | 0.0188 | 3.48 |
| `s2_mv_subset` | 1.6x | 0.012 | 3.49 |
| `cm_gamma` | 1.59x | 0.0383 | 4.11 |
| `sw_ar` | 1.59x | 0.0144 | 4.25 |
| `ch11_m11_7` | 1.58x | 0.00728 | 2.85 |
| `bym2_offset_only` | 1.58x | 0.0917 | 4.5 |
| `cm_choco` | 1.58x | 0.112 | 5.15 |
| `sw_arma` | 1.56x | 0.0159 | 4.34 |
| `cm_lba1` | 1.56x | 0.0616 | 4.97 |
| `cm_lnr_bench` | 1.55x | 0.545 | 8.96 |
| `surgical_model` | 1.54x | 0.00871 | 3.43 |
| `covid19imperial_v3` | 1.52x | 1.3 | 7.6 |
| `sw_mv_norescor` | 1.51x | 0.0119 | 3.49 |
| `sw_gaussian` | 1.5x | 0.00977 | 3.31 |
| `prophet` | 1.5x | 0.0945 | 5.68 |
| `sw_cratio_cs` | 1.49x | 0.204 | 5.3 |
| `i320_sratio_cs` | 1.48x | 0.201 | 5.58 |
| `s2_categorical_re` | 1.48x | 0.0252 | 5.45 |
| `lsat_model` | 1.48x | 0.0969 | 3.86 |
| `sw_gamma` | 1.48x | 0.0114 | 3.76 |
| `sw_re_negbin` | 1.46x | 0.015 | 4.76 |
| `gp_regr` | 1.46x | 0.0119 | 5.46 |
| `s2_unstr` | 1.45x | 0.0372 | 8.7 |
| `aalto_binom` | 1.44x | 0.00671 | 2.37 |
| `s2_rate` | 1.44x | 0.0102 | 3.6 |
| `covid19imperial_v2` | 1.43x | 1.31 | 7.57 |
| `Mb_model` | 1.43x | 0.113 | 3.39 |
| `s2_zi_asymlaplace` | 1.42x | 0.0195 | 3.67 |
| `gp_pois_regr` | 1.41x | 0.0114 | 5.64 |
| `s2_cumulative_probit` | 1.41x | 0.0178 | 3.69 |
| `Rate_1_model` | 1.4x | 0.00622 | 2.37 |
| `cm_lba2` | 1.39x | 0.0886 | 7.45 |
| `sw_exgaussian` | 1.38x | 0.0118 | 3.87 |
| `ch14_m14_2` | 1.38x | 0.0467 | 7.11 |
| `i319_negbin_re` | 1.37x | 0.031 | 4.79 |
| `ch11_m11_9` | 1.37x | 0.00751 | 2.6 |
| `Rate_4_model` | 1.37x | 0.00633 | 2.51 |
| `s2_mixture_theta` | 1.37x | 0.0268 | 4.47 |
| `aalto_bern` | 1.36x | 0.00655 | 2.56 |
| `aalto_binomb` | 1.36x | 0.00588 | 2.42 |
| `cm_exwald` | 1.36x | 0.108 | 5.98 |
| `hmm_example` | 1.35x | 0.0596 | 4.14 |
| `sw_cumulative_cs` | 1.34x | 0.19 | 5.17 |
| `irt_2pl` | 1.34x | 0.0436 | 4.27 |
| `s2_zoi_beta` | 1.34x | 0.0233 | 3.77 |
| `GLM_Binomial_model` | 1.34x | 0.00937 | 3.28 |
| `i320_gp_expquad` | 1.34x | 0.0326 | 7.31 |
| `Rate_2_model` | 1.33x | 0.00677 | 2.55 |
| `wells_dist` | 1.32x | 0.0556 | 3.07 |
| `sw_gp` | 1.31x | 0.0495 | 7.32 |
| `ch11_m_pois` | 1.3x | 0.0069 | 2.52 |
| `s2_zi_beta` | 1.3x | 0.0226 | 3.72 |
| `aalto_binom2` | 1.3x | 0.00646 | 2.5 |
| `Rate_3_model` | 1.29x | 0.0069 | 2.4 |
| `nn_rbm1bJ10` | 1.29x | 0.331 | 5.85 |
| `ch12_m12_1` | 1.28x | 0.00919 | 3.19 |
| `ch14_m14_6x` | 1.28x | 0.184 | 6.93 |
| `ch12_m12_2` | 1.26x | 0.0107 | 3.97 |
| `Rate_5_model` | 1.26x | 0.00735 | 2.49 |
| `sw_mixture` | 1.26x | 0.0243 | 4.74 |
| `ch12_m12_6` | 1.26x | 4.11 | 9.31 |
| `sw_beta` | 1.25x | 0.0123 | 3.9 |
| `blr` | 1.23x | 0.0101 | 3.06 |
| `ch14_m14_1` | 1.22x | 0.0204 | 6.9 |
| `ch14_m14_6` | 1.21x | 0.193 | 6.96 |
| `sw_mv_rescor` | 1.21x | 0.0249 | 6.11 |
| `s2_custom_vint` | 1.2x | 0.0195 | 3.54 |
| `ch14_m14_8nc` | 1.19x | 0.0166 | 5.99 |
| `logistic_regression_rhs` | 1.19x | 0.105 | 5.14 |
| `sw_categorical` | 1.17x | 0.0128 | 4.37 |
| `sw_weibull` | 1.16x | 0.0123 | 3.92 |
| `cm_rdm` | 1.16x | 0.226 | 10.8 |
| `sw_bernoulli` | 1.16x | 0.00863 | 3.27 |
| `s2_frechet` | 1.15x | 0.0132 | 3.95 |
| `sw_poisson` | 1.14x | 0.0095 | 3.11 |
| `s2_multinomial` | 1.14x | 0.0289 | 3.95 |
| `ch14_m14_7` | 1.14x | 0.0744 | 8.86 |
| `sw_binomial` | 1.14x | 0.0121 | 3.49 |
| `ch14_m14_10` | 1.12x | 0.825 | 5.15 |
| `cm_lnr` | 1.12x | 0.308 | 14 |
| `ch14_m14_9` | 1.11x | 0.762 | 5.07 |
| `Survey_model` | 1.11x | 0.131 | 3.09 |
| `sw_sratio` | 1.1x | 0.139 | 4.05 |
| `i320_sratio_plain` | 1.1x | 0.14 | 4.18 |
| `sw_mono` | 1.1x | 0.0158 | 4.13 |
| `ch14_m14_8` | 1.09x | 0.0165 | 4.77 |
| `sw_negbinomial` | 1.09x | 0.0123 | 3.41 |
| `sw_cratio` | 1.09x | 0.142 | 4.04 |
| `sw_trunc` | 1.08x | 0.0225 | 3.54 |
| `s2_beta_binomial` | 1.08x | 0.0182 | 3.7 |
| `s2_hurdle_negbin` | 1.07x | 0.0276 | 3.51 |
| `nn_rbm1bJ100` | 1.07x | 870 | 933 |
| `i319_pois_fixed` | 1.06x | 0.0137 | 3.13 |
| `wells_dist100_model` | 1.05x | 0.0425 | 3.23 |
| `s2_discrete_weibull` | 1.05x | 0.0173 | 3.51 |
| `s2_dirichlet` | 1.03x | 0.0362 | 4.39 |
| `diamonds` | 1.03x | 0.0903 | 3.49 |
| `s2_mo_simo_prior` | 1.02x | 0.0166 | 4.1 |
| `s2_weights_trunc` | 1.02x | 0.0232 | 3.59 |
| `wells_dae_c_model` | 1.02x | 0.0475 | 3.41 |
| `one_comp_mm_elim_abs` | 1.02x | 0.989 | 4.8 |
| `wells_dae_inter_model` | 1.02x | 0.0522 | 3.4 |
| `iohmm_reg` | 1.02x | 0.868 | 6.82 |
| `hmm_drive_0` | 1.01x | 0.335 | 5.1 |
| `nes_logit_model` | 1.01x | 0.0201 | 3.18 |
| `hmm_drive_1` | 1.01x | 0.362 | 5.08 |
| `hmm_gaussian` | 1.01x | 0.713 | 5.39 |
| `wells_daae_c_model` | 1.01x | 0.0504 | 3.42 |
| `wells_dist100ars_model` | 1.01x | 0.0453 | 3.27 |
| `wells_dae_model` | 1x | 0.05 | 3.28 |
| `wells_interaction_model` | 1x | 0.0496 | 3.3 |
| `i319_negbin_fixed` | 1x | 0.0254 | 3.41 |
| `ch15_m15_7` | 1x | 0.0336 | 7.62 |
| `aalto_poisson_simple` | 0.998x | 0.0114 | 2.67 |
| `i320_pois_trunc_both` | 0.987x | 0.0752 | 3.58 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.987x | 0.0495 | 3.36 |
| `bones_model` | 0.981x | 0.132 | 3.6 |
| `i320_pois_trunc_ub` | 0.972x | 0.0611 | 3.5 |
| `s2_cosy` | 0.968x | 0.0315 | 6.72 |
| `sw_cumulative` | 0.964x | 0.0541 | 3.78 |
| `s2_wiener` | 0.964x | 0.0733 | 3.75 |
| `cm_betadiscrete` | 0.964x | 3.98 | 8.12 |
| `s2_logistic_normal` | 0.958x | 0.0543 | 6.73 |
| `s2_ar_cov` | 0.953x | 0.0338 | 7.61 |
| `s2_fcor` | 0.929x | 0.0378 | 4.89 |
| `cm_ddm` | 0.904x | 39.5 | 44.9 |
| `i320_gp_matern32` | 0.853x | 0.0782 | 7.23 |
| `ch14_m14_11` | 0.847x | 1.68 | 5.82 |
| `garch11` | 0.82x | 0.0266 | 2.95 |
| `aalto_gpareto` | 0.756x | 0.0136 | 3.06 |
| `arma11` | 0.672x | 0.022 | 2.98 |
| `cm_invgaussian` | 0.429x | 0.386 | 33.8 |
| `s2_com_poisson` | 0.203x | 0.52 | 4.76 |

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
