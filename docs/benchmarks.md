# Benchmarks

Stanli runs Stan models without a per-model C++ build. This page compares it
with CmdStan on **<!--gen:benchmark_models-->343<!--/gen--> application models**.

## TL;DR

- The median model's gradient is **<!--gen:corpus_median-->2.20x<!--/gen--> faster** in Stanli than in
  CmdStan. Stanli is as fast or faster on <!--gen:corpus_at_par-->324<!--/gen--> of the
  <!--gen:corpus_n_grad-->336<!--/gen--> models that pass the numerical check.
- In the total-sampling column, which adds setup to the gradients a full
  1,000 warmup + 1,000 draw run takes, the median model is <!--gen:corpus_total_median-->137x<!--/gen--> faster, mostly because
  CmdStan builds C++ for each model. The longer the run, the closer this gets
  to the gradient speedup.
- Regressions, GLMs and IRT models gain the most. Models dominated by dense
  linear algebra, ODE solvers or sequential recurrences land near parity or
  run slower.
- Models that fail the numerical check are listed under each table with the
  reason.
- [Fast mode](#fast-mode), measured separately on an earlier run, has its own
  section.

The results tables come first. [How we measure](#how-we-measure) and
[reproduction](#reproduction) are appendices at the end.

<a id="full-corpus"></a>

## Full corpus results

Every application model uses the same settings. Language-conformance fixtures
belong to the [numerical corpus](corpus-status.md) and are not timed here.
CmdStan uses its default compiler settings. The `CmdStan / Stanli` column is
how many times faster Stanli's gradient is; complete results are sorted by it,
fastest first. Total sampling is each engine's setup time plus the warm gradients of a full run, with the gradient count
measured once per model; the benchmark itself runs no sampler ([details](#total-sampling-estimate)).
Incomplete results follow each table.

<!--gen:benchmark_catalog-->
Run `ebc312444363220e` (2026-10-20): 343 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 9 of 343 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 31.6x | 0.376 | 14.5 |
| `gpcm_latent_reg_irt` | 12.5x | 0.689 | 12.5 |
| `grsm_latent_reg_irt` | 11.9x | 0.381 | 9.22 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.5x | 0.169 | 4.48 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11.1x | 0.176 | 4.69 |
| `radon_county_intercept` | 10.1x | 0.277 | 5.27 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10x | 0.285 | 5.36 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.94x | 0.283 | 5.58 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.82x | 0.289 | 5.58 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.78x | 0.285 | 5.35 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.47x | 0.406 | 6.3 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.33x | 0.31 | 5.55 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.19x | 0.415 | 6.42 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.16x | 0.322 | 5.83 |
| `aalto_poisson_hurdle` | 8.84x | 0.0627 | 3.27 |
| `radon_pooled` | 8.62x | 0.22 | 4.5 |
| `dogs` | 7.91x | 0.0681 | 3.98 |
| `s2_custom_vreal` | 7.87x | 0.0132 | 3.51 |
| `rats_model` | 7.59x | 0.0147 | 3.13 |
| `arK` | 7.33x | 0.0178 | 3.42 |
| `ch12_m12_3_alt` | 7.04x | 0.0211 | 2.61 |
| `ch12_m12_3` | 6.8x | 0.0223 | 2.61 |
| `M0_model` | 6.45x | 0.0223 | 2.78 |
| `ch16_m16_1` | 6.28x | 0.032 | 3.11 |
| `Mt_model` | 5.49x | 0.0281 | 3.54 |
| `sw_nonlinear` | 5.32x | 0.0137 | 3.9 |
| `ctsem_ctsm_400` | 5.17x | 20.5 | 190 |
| `dogs_log` | 5.09x | 0.0662 | 3.48 |
| `ctsem_ctsm` | 4.99x | 2.72 | 98.3 |
| `ch09_m5_8s` | 4.96x | 0.00931 | 2.86 |
| `ch09_m5_8s2` | 4.74x | 0.00942 | 2.89 |
| `ch14_m14_5` | 4.61x | 0.0194 | 2.9 |
| `logmesquite_logvas` | 4.5x | 0.0117 | 3.4 |
| `s2_cumulative_cauchit` | 4.48x | 0.0188 | 3.69 |
| `ch15_m15_1` | 4.35x | 0.0103 | 3.2 |
| `mesquite` | 4.34x | 0.0106 | 3.1 |
| `ch15_m15_2` | 4.18x | 0.0111 | 3.46 |
| `s2_nl_noloop` | 4.18x | 0.0133 | 3.93 |
| `logmesquite_logvash` | 4.17x | 0.0107 | 3.35 |
| `s2_car_icar` | 4.15x | 0.0142 | 4.41 |
| `logmesquite` | 4.15x | 0.0129 | 3.17 |
| `logmesquite_logva` | 4.11x | 0.00993 | 3.18 |
| `logmesquite_logvolume` | 4.06x | 0.00887 | 3.01 |
| `nes` | 3.99x | 0.105 | 3.6 |
| `election88_full` | 3.99x | 1.21 | 8.9 |
| `s2_logistic_normal` | 3.97x | 0.0441 | 6.44 |
| `Mth_model` | 3.88x | 0.149 | 4.83 |
| `sw_me` | 3.85x | 0.0157 | 4.15 |
| `ch14_m14_4` | 3.85x | 0.0159 | 2.83 |
| `s2_nlf` | 3.78x | 0.0161 | 3.99 |
| `kidscore_interaction_c2` | 3.74x | 0.0209 | 3.05 |
| `s2_me2_nomecor` | 3.71x | 0.0176 | 4.47 |
| `ch14_m14_4x` | 3.7x | 0.0155 | 2.83 |
| `sw_spline_s` | 3.7x | 0.0154 | 4.47 |
| `ch09_m9_1` | 3.69x | 0.0111 | 3.11 |
| `sw_dist_sigma` | 3.69x | 0.0156 | 3.84 |
| `GLMM1_model` | 3.67x | 0.0603 | 3.43 |
| `s2_s_cc` | 3.64x | 0.0133 | 4.29 |
| `kilpisjarvi` | 3.59x | 0.00877 | 2.83 |
| `ch09_m9_1_chains4` | 3.58x | 0.0107 | 3.11 |
| `sw_se` | 3.57x | 0.0119 | 3.53 |
| `aalto_grp_aov` | 3.57x | 0.0075 | 3.07 |
| `s2_dist_sigma_re` | 3.56x | 0.016 | 4.91 |
| `sw_weights` | 3.54x | 0.0141 | 3.53 |
| `kidscore_interaction_c` | 3.54x | 0.0209 | 3.08 |
| `kidscore_mom_work` | 3.53x | 0.0215 | 3 |
| `kidscore_interaction_z` | 3.53x | 0.0208 | 3.16 |
| `kidscore_interaction` | 3.47x | 0.0221 | 3.08 |
| `s2_mm_weights` | 3.43x | 0.0175 | 4.74 |
| `logearn_interaction_z` | 3.43x | 0.0476 | 3.26 |
| `logearn_height_male` | 3.42x | 0.0359 | 3.04 |
| `s2_mm` | 3.42x | 0.0182 | 4.74 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.41x | 0.0443 | 4.97 |
| `logearn_interaction` | 3.39x | 0.0468 | 3.13 |
| `s2_shifted_lognormal` | 3.38x | 0.0142 | 3.93 |
| `ch16_m16_4` | 3.37x | 0.0158 | 2.96 |
| `sw_lognormal` | 3.35x | 0.0127 | 3.71 |
| `s2_s_by` | 3.35x | 0.0295 | 5.9 |
| `s2_cox_cens` | 3.35x | 0.0192 | 5.11 |
| `s2_gp_approx` | 3.34x | 0.0185 | 5.23 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.34x | 0.00819 | 3.3 |
| `logearn_logheight_male` | 3.34x | 0.0362 | 3.02 |
| `s2_car` | 3.3x | 0.0176 | 4.38 |
| `ch15_m15_5` | 3.28x | 0.0107 | 3.24 |
| `kidscore_momhsiq` | 3.27x | 0.0176 | 2.97 |
| `s2_car_esicar` | 3.24x | 0.0178 | 4.5 |
| `s2_gp_by_approx` | 3.24x | 0.0249 | 6.03 |
| `sw_spline_t2` | 3.23x | 0.0187 | 4.97 |
| `radon_county` | 3.22x | 0.146 | 3.56 |
| `i319_gauss_re` | 3.2x | 0.0194 | 4.59 |
| `s2_t2_by` | 3.17x | 0.0271 | 5.95 |
| `s2_gr_by` | 3.14x | 0.0167 | 4.95 |
| `ch15_m15_9` | 3.11x | 0.0214 | 3.04 |
| `aalto_grp_prior_mean` | 3.11x | 0.00771 | 3.18 |
| `s2_cox` | 3.1x | 0.0172 | 4.19 |
| `ch15_m15_8` | 3.1x | 0.0199 | 2.92 |
| `ch15_m15_6` | 3.04x | 0.00876 | 3.02 |
| `sw_acat` | 3.04x | 0.258 | 4.63 |
| `sw_re_gauss` | 3.03x | 0.0162 | 4.57 |
| `s2_cumulative_cloglog` | 2.93x | 0.0177 | 3.7 |
| `kidscore_momhs` | 2.92x | 0.0148 | 2.9 |
| `logearn_height` | 2.9x | 0.0268 | 2.91 |
| `log10earn_height` | 2.9x | 0.0275 | 2.87 |
| `earn_height` | 2.89x | 0.0262 | 2.87 |
| `eight_schools_centered` | 2.88x | 0.00707 | 3.01 |
| `s2_gr_student` | 2.87x | 0.0177 | 5.2 |
| `aalto_lin` | 2.87x | 0.00825 | 3.1 |
| `Mtbh_model` | 2.87x | 0.105 | 5.22 |
| `sesame_one_pred_a` | 2.85x | 0.0129 | 2.83 |
| `pilots` | 2.85x | 0.0119 | 3.4 |
| `bym2_offset_only` | 2.84x | 0.201 | 4.75 |
| `GLMM_Poisson_model` | 2.84x | 0.0112 | 3.84 |
| `aalto_grp_prior_mean_var` | 2.84x | 0.00941 | 3.91 |
| `blr` | 2.83x | 0.00869 | 3.17 |
| `s2_mi_lognormal` | 2.81x | 0.0166 | 4.64 |
| `dogs_nonhierarchical` | 2.8x | 0.104 | 7.13 |
| `sw_student` | 2.8x | 0.0139 | 3.88 |
| `s2_hurdle_cumulative` | 2.79x | 0.0266 | 4.24 |
| `sw_hurdle_lognormal` | 2.78x | 0.0196 | 3.59 |
| `aalto_lin_std` | 2.77x | 0.00803 | 3.3 |
| `s2_mmc` | 2.76x | 0.0244 | 6.99 |
| `GLM_Poisson_model` | 2.76x | 0.0092 | 3.3 |
| `ch13_m13_4nc` | 2.7x | 0.055 | 3.44 |
| `kidscore_momiq` | 2.69x | 0.014 | 2.9 |
| `sw_gamma` | 2.68x | 0.0142 | 3.77 |
| `cm_geg` | 2.68x | 0.143 | 4.21 |
| `ch12_m12_7` | 2.67x | 4.87 | 16.4 |
| `accel_gp` | 2.64x | 0.0377 | 5.46 |
| `sw_hurdle_pois` | 2.63x | 0.0226 | 3.42 |
| `hierarchical_gp` | 2.63x | 0.115 | 9.27 |
| `ch12_m12_5` | 2.63x | 5.06 | 16.6 |
| `cm_exwald` | 2.61x | 0.291 | 4.8 |
| `cm_exgaussian` | 2.6x | 0.0881 | 3.86 |
| `s2_gev` | 2.57x | 0.0229 | 3.82 |
| `s2_sar` | 2.57x | 0.0297 | 4.12 |
| `losscurve_sislob` | 2.53x | 0.0186 | 4.31 |
| `accel_splines` | 2.52x | 0.0328 | 4.45 |
| `sw_acat_cs` | 2.51x | 0.369 | 5.86 |
| `s2_me2` | 2.48x | 0.0224 | 6.77 |
| `s2_sar_error` | 2.47x | 0.0313 | 4.17 |
| `ldaK5` | 2.45x | 12.8 | 34 |
| `i319_pois_re2` | 2.43x | 0.0388 | 4.88 |
| `ch15_m15_3` | 2.39x | 0.0735 | 2.81 |
| `ch11_m11_10` | 2.37x | 0.00849 | 3.25 |
| `Mh_model` | 2.36x | 0.101 | 3.49 |
| `ch14_m14_3` | 2.36x | 0.063 | 6 |
| `i320_mi_nhanes` | 2.35x | 0.0196 | 4.42 |
| `s2_mv_shared_re` | 2.34x | 0.025 | 7.1 |
| `ch15_m15_4` | 2.34x | 0.0625 | 2.75 |
| `dogs_hierarchical` | 2.34x | 0.103 | 2.98 |
| `seeds_centered_model` | 2.33x | 0.0135 | 3.87 |
| `lsat_model` | 2.32x | 0.231 | 4.25 |
| `s2_rate` | 2.32x | 0.013 | 3.62 |
| `sw_re_bern` | 2.31x | 0.0168 | 4.67 |
| `sw_skewnormal` | 2.28x | 0.018 | 3.99 |
| `aalto_lin_std_t` | 2.27x | 0.0096 | 3.51 |
| `ch09_m9_2` | 2.27x | 0.00656 | 2.53 |
| `ch13_m13_4b` | 2.25x | 0.0453 | 3.38 |
| `ch09_m9_5` | 2.24x | 0.00761 | 2.55 |
| `ch09_m9_4` | 2.24x | 0.00705 | 2.57 |
| `dugongs_model` | 2.24x | 0.0102 | 3.02 |
| `ch09_m9_3` | 2.24x | 0.00699 | 2.53 |
| `extra_hurdle_poisson` | 2.24x | 0.00819 | 2.61 |
| `ldaK2` | 2.23x | 0.265 | 4.26 |
| `sw_re_slope` | 2.22x | 0.0219 | 6.81 |
| `normal_mixture` | 2.22x | 0.226 | 3.08 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.21x | 0.245 | 3.64 |
| `sw_zi_poisson` | 2.2x | 0.0193 | 3.41 |
| `ch11_m11_5` | 2.19x | 0.0474 | 3.2 |
| `sw_vonmises` | 2.19x | 0.015 | 3.7 |
| `ch13_m13_4` | 2.18x | 0.0494 | 3.5 |
| `ch13_m13_7` | 2.17x | 0.00677 | 2.32 |
| `2pl_latent_reg_irt` | 2.16x | 0.345 | 6.15 |
| `ch13_m13_6` | 2.15x | 0.0494 | 3.46 |
| `low_dim_gauss_mix` | 2.14x | 0.242 | 3.7 |
| `ch09_mp` | 2.06x | 0.006 | 2.33 |
| `multi_occupancy` | 2.04x | 0.177 | 6.37 |
| `ch13_m13_7nc` | 2.04x | 0.00677 | 2.4 |
| `seeds_stanified_model` | 2.03x | 0.0131 | 3.57 |
| `s2_index_mi` | 2.03x | 0.0172 | 4.27 |
| `prophet` | 2.02x | 0.204 | 5.36 |
| `seeds_model` | 2.01x | 0.0141 | 3.67 |
| `sw_re_pois` | 2x | 0.0172 | 4.52 |
| `ch11_m11_4` | 1.99x | 0.0427 | 3.08 |
| `ch11_m11_6` | 1.98x | 0.0095 | 3.05 |
| `ch13_m13_5` | 1.97x | 0.0433 | 3.32 |
| `normal_mixture_k` | 1.95x | 1.03 | 5.38 |
| `sw_exgaussian` | 1.94x | 0.0158 | 3.91 |
| `soil_incubation` | 1.93x | 0.299 | 4.03 |
| `cm_lognormal` | 1.92x | 0.18 | 4.49 |
| `cm_bisa` | 1.9x | 0.0816 | 4.16 |
| `cm_loggamma` | 1.89x | 0.126 | 4.36 |
| `sw_ma` | 1.87x | 0.021 | 4.28 |
| `ch11_m11_8` | 1.87x | 0.0088 | 2.96 |
| `ch13_m13_1` | 1.85x | 0.0111 | 2.98 |
| `sw_arma` | 1.85x | 0.024 | 4.37 |
| `ch13_m13_3` | 1.83x | 0.0122 | 3.03 |
| `sw_hurdle_gamma` | 1.82x | 0.0244 | 3.59 |
| `s2_cens_interval` | 1.82x | 0.0196 | 3.78 |
| `sw_zi_binomial` | 1.8x | 0.0259 | 3.44 |
| `i319_pois_re` | 1.8x | 0.0307 | 4.54 |
| `sw_mi` | 1.79x | 0.0162 | 3.86 |
| `ch13_m13_2` | 1.79x | 0.0117 | 3.14 |
| `cm_logweibull` | 1.78x | 0.0933 | 4.15 |
| `sw_ar` | 1.77x | 0.0202 | 4.26 |
| `ch11_m11_11` | 1.76x | 0.00946 | 3.57 |
| `wells_dist` | 1.75x | 0.128 | 3.14 |
| `cm_choco` | 1.73x | 0.244 | 4.86 |
| `s2_threading` | 1.73x | 0.0139 | 3.43 |
| `cm_lnr` | 1.7x | 0.518 | 5.04 |
| `s2_mv_subset` | 1.7x | 0.015 | 3.48 |
| `sw_beta` | 1.69x | 0.0207 | 3.91 |
| `cm_invweibull` | 1.67x | 0.104 | 4.16 |
| `cm_betagate` | 1.67x | 0.189 | 4.27 |
| `cm_lba1` | 1.67x | 0.12 | 4.51 |
| `sw_asymlaplace` | 1.66x | 0.0233 | 3.57 |
| `cm_weibull` | 1.65x | 0.101 | 4.22 |
| `sw_gaussian` | 1.65x | 0.0125 | 3.31 |
| `lotka_volterra` | 1.65x | 0.421 | 5 |
| `cm_logstudent` | 1.64x | 0.114 | 4.32 |
| `cm_gamma` | 1.62x | 0.102 | 4.18 |
| `sw_mv_norescor` | 1.61x | 0.0152 | 3.49 |
| `s2_zoi_beta` | 1.61x | 0.0358 | 3.72 |
| `GLM_Binomial_model` | 1.6x | 0.0123 | 3.3 |
| `sw_zi_negbin` | 1.6x | 0.0302 | 3.49 |
| `cm_invgamma` | 1.59x | 0.104 | 4.16 |
| `ch11_m11_7` | 1.58x | 0.00795 | 2.85 |
| `sir` | 1.58x | 0.716 | 5.23 |
| `surgical_model` | 1.58x | 0.0112 | 3.45 |
| `s2_zi_beta` | 1.58x | 0.0353 | 3.69 |
| `s2_categorical_re` | 1.55x | 0.0411 | 5.48 |
| `s2_mixture_theta` | 1.55x | 0.0452 | 4.59 |
| `cm_lba2` | 1.51x | 0.199 | 4.89 |
| `gp_pois_regr` | 1.5x | 0.0175 | 5.79 |
| `s2_frechet` | 1.5x | 0.0213 | 4.02 |
| `s2_unstr` | 1.5x | 0.0516 | 7.55 |
| `sw_weibull` | 1.49x | 0.0209 | 3.92 |
| `cm_lnr_bench` | 1.48x | 1.06 | 5.97 |
| `sw_re_negbin` | 1.48x | 0.023 | 4.78 |
| `gp_regr` | 1.47x | 0.019 | 5.5 |
| `Rate_4_model` | 1.47x | 0.00687 | 2.52 |
| `ch11_m11_9` | 1.47x | 0.00748 | 2.59 |
| `covid19imperial_v3` | 1.46x | 2.04 | 8.74 |
| `covid19imperial_v2` | 1.45x | 2.05 | 8.73 |
| `ch12_m12_6` | 1.45x | 9.64 | 18 |
| `Mb_model` | 1.44x | 0.219 | 3.55 |
| `s2_gp_by_gr` | 1.44x | 0.0652 | 9.17 |
| `s2_zi_asymlaplace` | 1.44x | 0.0304 | 3.63 |
| `hmm_example` | 1.43x | 0.117 | 4.24 |
| `ch14_m14_2` | 1.43x | 0.0958 | 7.2 |
| `Rate_2_model` | 1.42x | 0.0075 | 2.57 |
| `i320_gp_expquad` | 1.41x | 0.0649 | 7.14 |
| `aalto_binomb` | 1.39x | 0.0065 | 2.41 |
| `ch15_m15_7` | 1.39x | 0.0559 | 7.2 |
| `aalto_binom` | 1.38x | 0.00582 | 2.4 |
| `sw_mixture` | 1.37x | 0.0408 | 4.86 |
| `aalto_binom2` | 1.37x | 0.00658 | 2.51 |
| `Rate_1_model` | 1.36x | 0.00619 | 2.39 |
| `ch11_m_pois` | 1.36x | 0.00688 | 2.53 |
| `ch14_m14_8nc` | 1.35x | 0.0245 | 5.7 |
| `sw_binomial` | 1.34x | 0.0198 | 3.59 |
| `aalto_bern` | 1.34x | 0.00661 | 2.57 |
| `ch12_m12_2` | 1.33x | 0.0122 | 3.99 |
| `ch14_m14_1` | 1.33x | 0.0349 | 6.91 |
| `sw_mono` | 1.32x | 0.0222 | 3.99 |
| `Rate_3_model` | 1.32x | 0.00682 | 2.4 |
| `i319_negbin_re` | 1.31x | 0.061 | 4.81 |
| `Rate_5_model` | 1.31x | 0.00704 | 2.49 |
| `ch12_m12_1` | 1.31x | 0.0129 | 3.24 |
| `s2_mo_simo_prior` | 1.31x | 0.0237 | 4 |
| `ch14_m14_6x` | 1.3x | 0.44 | 7.2 |
| `ch14_m14_6` | 1.29x | 0.442 | 7.21 |
| `sw_gp` | 1.27x | 0.109 | 7.17 |
| `ch14_m14_9` | 1.26x | 1.74 | 6.23 |
| `nn_rbm1bJ10` | 1.25x | 0.806 | 6.5 |
| `s2_beta_binomial` | 1.25x | 0.0342 | 3.73 |
| `sw_sratio` | 1.25x | 0.314 | 3.99 |
| `s2_custom_vint` | 1.23x | 0.0376 | 3.59 |
| `sw_cratio` | 1.22x | 0.317 | 4.01 |
| `cm_rdm` | 1.22x | 0.306 | 4.65 |
| `sw_bernoulli` | 1.22x | 0.0117 | 3.3 |
| `ch14_m14_10` | 1.21x | 1.91 | 6.37 |
| `ch14_m14_7` | 1.21x | 0.158 | 9 |
| `i320_sratio_plain` | 1.2x | 0.322 | 4.15 |
| `sw_mv_rescor` | 1.2x | 0.0454 | 6.22 |
| `ch14_m14_8` | 1.2x | 0.0237 | 4.49 |
| `s2_cumulative_probit` | 1.18x | 0.0333 | 3.75 |
| `sw_categorical` | 1.15x | 0.0184 | 4.34 |
| `Survey_model` | 1.14x | 0.303 | 3.31 |
| `i320_sratio_cs` | 1.14x | 0.476 | 5.53 |
| `sw_poisson` | 1.14x | 0.0132 | 3.11 |
| `sw_cratio_cs` | 1.14x | 0.477 | 5.24 |
| `s2_discrete_weibull` | 1.12x | 0.0294 | 3.52 |
| `s2_multinomial` | 1.11x | 0.06 | 4.06 |
| `garch11` | 1.1x | 0.0491 | 3 |
| `ch14_m14_11` | 1.1x | 3.17 | 7.37 |
| `hmm_gaussian` | 1.08x | 1.48 | 6.25 |
| `s2_weights_trunc` | 1.08x | 0.0452 | 3.68 |
| `s2_hurdle_negbin` | 1.08x | 0.0491 | 3.53 |
| `sw_trunc` | 1.08x | 0.043 | 3.61 |
| `nn_rbm1bJ100` | 1.08x | 2.19e+03 | 2.36e+03 |
| `sw_negbinomial` | 1.07x | 0.0184 | 3.4 |
| `iohmm_reg` | 1.04x | 1.91 | 7.8 |
| `hmm_drive_1` | 1.04x | 0.802 | 5.57 |
| `s2_dirichlet` | 1.04x | 0.0756 | 4.44 |
| `i320_pois_trunc_ub` | 1.03x | 0.14 | 3.63 |
| `i319_negbin_fixed` | 1.03x | 0.0522 | 3.43 |
| `wells_dist100_model` | 1.03x | 0.0945 | 3.29 |
| `bones_model` | 1.02x | 0.279 | 3.78 |
| `aalto_poisson_simple` | 1.02x | 0.0126 | 2.67 |
| `wells_dist100ars_model` | 1.02x | 0.103 | 3.32 |
| `i319_pois_fixed` | 1.02x | 0.023 | 3.15 |
| `nes_logit_model` | 1.01x | 0.0415 | 3.17 |
| `diamonds` | 1.01x | 0.191 | 3.58 |
| `wells_dae_model` | 1.01x | 0.113 | 3.34 |
| `wells_dae_inter_model` | 1.01x | 0.118 | 3.45 |
| `s2_wiener` | 1.01x | 0.139 | 3.8 |
| `wells_dae_c_model` | 1.01x | 0.108 | 3.46 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 0.114 | 3.41 |
| `wells_interaction_model` | 1.01x | 0.113 | 3.37 |
| `one_comp_mm_elim_abs` | 1.01x | 2.32 | 5.79 |
| `s2_cosy` | 1x | 0.0506 | 6.49 |
| `wells_daae_c_model` | 1x | 0.116 | 3.47 |
| `cm_betadiscrete` | 1x | 10.7 | 14.9 |
| `s2_ar_cov` | 0.999x | 0.0531 | 7.22 |
| `cm_ddm` | 0.999x | 850 | 858 |
| `arma11` | 0.998x | 0.037 | 3.03 |
| `sw_cumulative` | 0.98x | 0.121 | 3.89 |
| `i320_pois_trunc_both` | 0.976x | 0.18 | 3.71 |
| `sw_cumulative_cs` | 0.973x | 0.384 | 5.1 |
| `hmm_drive_0` | 0.968x | 0.778 | 5.53 |
| `s2_fcor` | 0.947x | 0.0806 | 4.99 |
| `i320_gp_matern32` | 0.93x | 0.169 | 7.1 |
| `aalto_gpareto` | 0.815x | 0.0139 | 2.9 |
| `cm_invgaussian` | 0.751x | 0.874 | 5.23 |
| `s2_com_poisson` | 0.246x | 1.73 | 4.09 |

Total sampling is an estimate: each engine's measured setup time + 5,000 × its warm gradient time, averaged over 10 evaluation points. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `hier_2pl` | — | — | — | failed; point 1: density/gradient mismatch: scaled error 7.06e-09 |
| `irt_2pl` | — | — | — | failed; point 1: density/gradient mismatch: scaled error 5.06e-09 |
| `kronecker_gp` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.000113 |
| `logistic_regression_rhs` | — | — | — | failed; point 3: density/gradient mismatch: scaled error 6.9e-09 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5594) |
| `s2_mi_trunc_lb` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 2.99e-07 |
| `sw_cens` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->

### CmdStan with stanc3 loop vectorization

This table repeats the comparison with stanc3's loop vectorization turned on
for CmdStan at `--O1`. The Stanli timings are the default run's.
[How this table is built](#loop-vectorization-comparison) is in the appendix.

<!--gen:benchmark_vectorized_catalog-->
Stanli timings are from the default run `ebc312444363220e`. The CmdStan timings with loop vectorization are reused from run `bd286a248ca3f26e` (2026-10-20). Each ratio is CmdStan's median gradient time divided by Stanli's, each averaged over 10 evaluation points.

CmdStan stanc and build times for 6 of 343 models were reused from the build cache and not remeasured in the runs that report them.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 30.9x | 0.376 | 14.2 |
| `gpcm_latent_reg_irt` | 12.8x | 0.689 | 13 |
| `grsm_latent_reg_irt` | 11.8x | 0.381 | 9.43 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.5x | 0.169 | 4.51 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11.2x | 0.176 | 4.71 |
| `aalto_poisson_hurdle` | 8.82x | 0.0627 | 3.27 |
| `radon_county_intercept` | 8.26x | 0.277 | 4.82 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.2x | 0.285 | 4.95 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.07x | 0.285 | 4.94 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8.04x | 0.283 | 5.12 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 8.01x | 0.289 | 5.13 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.73x | 0.31 | 5.13 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.45x | 0.406 | 5.65 |
| `s2_custom_vreal` | 7.41x | 0.0132 | 3.45 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.36x | 0.322 | 5.39 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.33x | 0.415 | 5.82 |
| `ch12_m12_3_alt` | 7.12x | 0.0211 | 2.6 |
| `ch12_m12_3` | 6.9x | 0.0223 | 2.6 |
| `rats_model` | 6.77x | 0.0147 | 3.11 |
| `M0_model` | 6.45x | 0.0223 | 2.77 |
| `ch16_m16_1` | 6.41x | 0.032 | 3.13 |
| `ctsem_ctsm_400` | 5.5x | 20.5 | 551 |
| `ctsem_ctsm` | 5.43x | 2.72 | 453 |
| `arK` | 4.99x | 0.0178 | 2.91 |
| `Mt_model` | 4.94x | 0.0281 | 3.56 |
| `Mth_model` | 4.11x | 0.149 | 4.79 |
| `s2_car_icar` | 4.01x | 0.0142 | 4.39 |
| `s2_cumulative_cauchit` | 3.85x | 0.0188 | 3.77 |
| `s2_logistic_normal` | 3.63x | 0.0441 | 6.83 |
| `dogs_log` | 3.61x | 0.0662 | 3.98 |
| `GLMM1_model` | 3.56x | 0.0603 | 3.39 |
| `sw_weights` | 3.56x | 0.0141 | 3.47 |
| `dogs` | 3.45x | 0.0681 | 4.34 |
| `sw_me` | 3.43x | 0.0157 | 4.27 |
| `s2_mm` | 3.37x | 0.0182 | 4.71 |
| `s2_mm_weights` | 3.36x | 0.0175 | 4.71 |
| `cm_exgaussian` | 3.31x | 0.0881 | 4.56 |
| `s2_car_esicar` | 3.28x | 0.0178 | 4.76 |
| `s2_gp_by_approx` | 3.27x | 0.0249 | 6.54 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.25x | 0.0443 | 4.97 |
| `s2_cox_cens` | 3.25x | 0.0192 | 5.01 |
| `s2_me2_nomecor` | 3.23x | 0.0176 | 4.55 |
| `i319_gauss_re` | 3.23x | 0.0194 | 4.62 |
| `s2_car` | 3.22x | 0.0176 | 4.76 |
| `aalto_grp_aov` | 3.12x | 0.0075 | 3.04 |
| `ch09_m9_1` | 3.11x | 0.0111 | 3.32 |
| `s2_gr_by` | 3.1x | 0.0167 | 4.95 |
| `s2_dist_sigma_re` | 3.08x | 0.016 | 5 |
| `logmesquite_logva` | 3.08x | 0.00993 | 3.33 |
| `ch09_m9_1_chains4` | 3.07x | 0.0107 | 3.32 |
| `logmesquite_logvolume` | 3.07x | 0.00887 | 3.06 |
| `logmesquite_logvash` | 3.07x | 0.0107 | 3.57 |
| `mesquite` | 3.06x | 0.0106 | 3.38 |
| `sw_re_gauss` | 3.06x | 0.0162 | 4.62 |
| `logmesquite_logvas` | 3.06x | 0.0117 | 3.65 |
| `logmesquite` | 3.05x | 0.0129 | 3.41 |
| `radon_county` | 2.97x | 0.146 | 3.52 |
| `ch15_m15_1` | 2.95x | 0.0103 | 3.42 |
| `s2_cox` | 2.94x | 0.0172 | 4.08 |
| `s2_hurdle_cumulative` | 2.9x | 0.0266 | 4.21 |
| `dogs_nonhierarchical` | 2.84x | 0.104 | 7.04 |
| `eight_schools_centered` | 2.83x | 0.00707 | 2.99 |
| `aalto_grp_prior_mean` | 2.83x | 0.00771 | 3.13 |
| `s2_gr_student` | 2.8x | 0.0177 | 5.3 |
| `ch15_m15_5` | 2.76x | 0.0107 | 3.67 |
| `cm_geg` | 2.72x | 0.143 | 4.9 |
| `sw_acat` | 2.72x | 0.258 | 4.6 |
| `Mtbh_model` | 2.72x | 0.105 | 5.16 |
| `sw_acat_cs` | 2.7x | 0.369 | 5.99 |
| `sw_hurdle_lognormal` | 2.69x | 0.0196 | 3.57 |
| `ch09_m5_8s` | 2.67x | 0.00931 | 3.07 |
| `sw_hurdle_pois` | 2.66x | 0.0226 | 3.43 |
| `kilpisjarvi` | 2.64x | 0.00877 | 2.8 |
| `aalto_grp_prior_mean_var` | 2.63x | 0.00941 | 3.84 |
| `ch15_m15_2` | 2.63x | 0.0111 | 3.67 |
| `aalto_lin` | 2.62x | 0.00825 | 3.05 |
| `s2_mmc` | 2.59x | 0.0244 | 7 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.58x | 0.00819 | 3.21 |
| `ch09_m5_8s2` | 2.57x | 0.00942 | 3.05 |
| `hierarchical_gp` | 2.57x | 0.115 | 9.32 |
| `aalto_lin_std` | 2.57x | 0.00803 | 3.26 |
| `ch15_m15_8` | 2.55x | 0.0199 | 2.92 |
| `ch12_m12_7` | 2.55x | 4.87 | 15.7 |
| `cm_exwald` | 2.54x | 0.291 | 6.44 |
| `ch15_m15_9` | 2.54x | 0.0214 | 3.16 |
| `ch12_m12_5` | 2.53x | 5.06 | 16.1 |
| `pilots` | 2.52x | 0.0119 | 3.49 |
| `s2_shifted_lognormal` | 2.49x | 0.0142 | 3.93 |
| `s2_s_cc` | 2.49x | 0.0133 | 4.23 |
| `ldaK5` | 2.49x | 12.8 | 34.5 |
| `s2_gp_approx` | 2.47x | 0.0185 | 5.51 |
| `nes` | 2.43x | 0.105 | 3.85 |
| `ch15_m15_6` | 2.42x | 0.00876 | 3.18 |
| `sw_spline_s` | 2.38x | 0.0154 | 4.37 |
| `ch14_m14_3` | 2.38x | 0.063 | 6.01 |
| `s2_me2` | 2.37x | 0.0224 | 6.69 |
| `i320_mi_nhanes` | 2.37x | 0.0196 | 4.47 |
| `s2_nlf` | 2.34x | 0.0161 | 4.35 |
| `s2_mv_shared_re` | 2.33x | 0.025 | 7.22 |
| `dogs_hierarchical` | 2.33x | 0.103 | 2.99 |
| `losscurve_sislob` | 2.32x | 0.0186 | 4.73 |
| `accel_gp` | 2.31x | 0.0377 | 6.69 |
| `s2_cumulative_cloglog` | 2.31x | 0.0177 | 3.79 |
| `kidscore_interaction_c` | 2.3x | 0.0209 | 3.22 |
| `kidscore_interaction_c2` | 2.3x | 0.0209 | 3.19 |
| `sw_re_bern` | 2.29x | 0.0168 | 4.66 |
| `sw_re_slope` | 2.29x | 0.0219 | 6.82 |
| `ch09_m9_4` | 2.29x | 0.00705 | 2.57 |
| `s2_gev` | 2.29x | 0.0229 | 3.86 |
| `s2_sar` | 2.29x | 0.0297 | 4.23 |
| `kidscore_mom_work` | 2.29x | 0.0215 | 3.15 |
| `normal_mixture` | 2.28x | 0.226 | 3.3 |
| `ch09_m9_5` | 2.28x | 0.00761 | 2.59 |
| `ch09_m9_2` | 2.28x | 0.00656 | 2.55 |
| `kidscore_interaction` | 2.27x | 0.0221 | 3.21 |
| `kidscore_interaction_z` | 2.27x | 0.0208 | 3.27 |
| `sw_skewnormal` | 2.27x | 0.018 | 3.92 |
| `logearn_logheight_male` | 2.25x | 0.0362 | 3.11 |
| `dugongs_model` | 2.25x | 0.0102 | 3.04 |
| `sw_vonmises` | 2.23x | 0.015 | 3.67 |
| `logearn_height_male` | 2.23x | 0.0359 | 3.1 |
| `extra_hurdle_poisson` | 2.21x | 0.00819 | 2.64 |
| `s2_sar_error` | 2.21x | 0.0313 | 4.22 |
| `kidscore_momhsiq` | 2.2x | 0.0176 | 3.09 |
| `ch14_m14_5` | 2.2x | 0.0194 | 3.08 |
| `ch09_m9_3` | 2.19x | 0.00699 | 2.53 |
| `s2_mi_lognormal` | 2.19x | 0.0166 | 4.58 |
| `GLMM_Poisson_model` | 2.19x | 0.0112 | 3.84 |
| `ch13_m13_4b` | 2.19x | 0.0453 | 3.39 |
| `ch11_m11_5` | 2.19x | 0.0474 | 3.18 |
| `i319_pois_re2` | 2.19x | 0.0388 | 4.9 |
| `Mh_model` | 2.19x | 0.101 | 3.51 |
| `sw_nonlinear` | 2.18x | 0.0137 | 3.89 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.18x | 0.245 | 3.63 |
| `ch13_m13_6` | 2.18x | 0.0494 | 3.47 |
| `ch13_m13_4` | 2.16x | 0.0494 | 3.52 |
| `ch13_m13_4nc` | 2.16x | 0.055 | 3.42 |
| `sw_lognormal` | 2.16x | 0.0127 | 3.66 |
| `logearn_interaction` | 2.15x | 0.0468 | 3.27 |
| `logearn_interaction_z` | 2.14x | 0.0476 | 3.37 |
| `low_dim_gauss_mix` | 2.14x | 0.242 | 3.71 |
| `aalto_lin_std_t` | 2.13x | 0.0096 | 3.47 |
| `ch13_m13_7` | 2.12x | 0.00677 | 2.32 |
| `s2_s_by` | 2.1x | 0.0295 | 5.72 |
| `sesame_one_pred_a` | 2.07x | 0.0129 | 2.88 |
| `sw_student` | 2.07x | 0.0139 | 3.79 |
| `accel_splines` | 2.06x | 0.0328 | 4.62 |
| `seeds_centered_model` | 2.06x | 0.0135 | 3.91 |
| `log10earn_height` | 2.06x | 0.0275 | 2.94 |
| `ch14_m14_4` | 2.06x | 0.0159 | 2.89 |
| `ch09_mp` | 2.05x | 0.006 | 2.31 |
| `radon_pooled` | 2.05x | 0.22 | 3.29 |
| `earn_height` | 2.04x | 0.0262 | 2.9 |
| `ch14_m14_4x` | 2.04x | 0.0155 | 2.89 |
| `ldaK2` | 2.03x | 0.265 | 4.18 |
| `logearn_height` | 2.02x | 0.0268 | 2.94 |
| `kidscore_momhs` | 2.02x | 0.0148 | 2.93 |
| `sw_re_pois` | 2.02x | 0.0172 | 4.52 |
| `kidscore_momiq` | 2.02x | 0.014 | 2.94 |
| `normal_mixture_k` | 2x | 1.03 | 5.38 |
| `GLM_Poisson_model` | 2x | 0.0092 | 3.32 |
| `ch13_m13_7nc` | 1.99x | 0.00677 | 2.38 |
| `sw_se` | 1.98x | 0.0119 | 3.45 |
| `cm_lnr` | 1.98x | 0.518 | 14.5 |
| `sw_zi_poisson` | 1.98x | 0.0193 | 3.45 |
| `ch11_m11_10` | 1.98x | 0.00849 | 3.25 |
| `s2_nl_noloop` | 1.97x | 0.0133 | 3.81 |
| `ch11_m11_4` | 1.97x | 0.0427 | 3.08 |
| `election88_full` | 1.96x | 1.21 | 7.44 |
| `sw_dist_sigma` | 1.96x | 0.0156 | 3.77 |
| `sw_spline_t2` | 1.94x | 0.0187 | 4.82 |
| `cm_lognormal` | 1.93x | 0.18 | 5.73 |
| `ch13_m13_5` | 1.92x | 0.0433 | 3.33 |
| `ch11_m11_6` | 1.92x | 0.0095 | 3.05 |
| `ch16_m16_4` | 1.89x | 0.0158 | 3.37 |
| `s2_index_mi` | 1.88x | 0.0172 | 4.19 |
| `s2_t2_by` | 1.88x | 0.0271 | 5.67 |
| `ch15_m15_3` | 1.86x | 0.0735 | 2.75 |
| `multi_occupancy` | 1.86x | 0.177 | 6.76 |
| `ch13_m13_1` | 1.86x | 0.0111 | 2.99 |
| `ch11_m11_8` | 1.86x | 0.0088 | 2.95 |
| `i319_pois_re` | 1.85x | 0.0307 | 4.58 |
| `soil_incubation` | 1.85x | 0.299 | 4.44 |
| `ch13_m13_2` | 1.84x | 0.0117 | 3.16 |
| `cm_loggamma` | 1.84x | 0.126 | 4.79 |
| `cm_bisa` | 1.82x | 0.0816 | 4.27 |
| `ch15_m15_4` | 1.81x | 0.0625 | 2.76 |
| `s2_cens_interval` | 1.81x | 0.0196 | 3.75 |
| `ch13_m13_3` | 1.81x | 0.0122 | 3.05 |
| `sw_mi` | 1.77x | 0.0162 | 3.85 |
| `sw_hurdle_gamma` | 1.76x | 0.0244 | 3.56 |
| `sw_asymlaplace` | 1.75x | 0.0233 | 3.63 |
| `cm_logweibull` | 1.75x | 0.0933 | 4.17 |
| `ch11_m11_11` | 1.72x | 0.00946 | 3.57 |
| `cm_betagate` | 1.71x | 0.189 | 4.49 |
| `sw_zi_binomial` | 1.71x | 0.0259 | 3.45 |
| `bym2_offset_only` | 1.68x | 0.201 | 4.7 |
| `seeds_stanified_model` | 1.68x | 0.0131 | 3.76 |
| `s2_threading` | 1.67x | 0.0139 | 3.38 |
| `sw_ar` | 1.65x | 0.0202 | 4.25 |
| `sw_arma` | 1.64x | 0.024 | 4.34 |
| `cm_logstudent` | 1.64x | 0.114 | 4.33 |
| `sw_ma` | 1.64x | 0.021 | 4.25 |
| `cm_invweibull` | 1.63x | 0.104 | 4.19 |
| `lotka_volterra` | 1.63x | 0.421 | 5.17 |
| `cm_weibull` | 1.63x | 0.101 | 4.2 |
| `cm_gamma` | 1.63x | 0.102 | 4.21 |
| `cm_lba1` | 1.63x | 0.12 | 5.05 |
| `2pl_latent_reg_irt` | 1.62x | 0.345 | 6.46 |
| `seeds_model` | 1.62x | 0.0141 | 3.77 |
| `cm_lnr_bench` | 1.62x | 1.06 | 9.77 |
| `s2_mv_subset` | 1.61x | 0.015 | 3.49 |
| `ch11_m11_7` | 1.58x | 0.00795 | 2.86 |
| `surgical_model` | 1.58x | 0.0112 | 3.44 |
| `sw_gaussian` | 1.58x | 0.0125 | 3.31 |
| `cm_invgamma` | 1.58x | 0.104 | 4.23 |
| `s2_categorical_re` | 1.56x | 0.0411 | 5.44 |
| `sw_zi_negbin` | 1.56x | 0.0302 | 3.51 |
| `sw_mv_norescor` | 1.54x | 0.0152 | 3.51 |
| `cm_choco` | 1.54x | 0.244 | 5.35 |
| `sir` | 1.54x | 0.716 | 5.18 |
| `sw_gamma` | 1.52x | 0.0142 | 3.75 |
| `s2_unstr` | 1.52x | 0.0516 | 8.75 |
| `lsat_model` | 1.51x | 0.231 | 4.08 |
| `prophet` | 1.5x | 0.204 | 5.87 |
| `sw_re_negbin` | 1.49x | 0.023 | 4.78 |
| `Mb_model` | 1.48x | 0.219 | 3.55 |
| `cm_lba2` | 1.48x | 0.199 | 7.67 |
| `ch11_m11_9` | 1.47x | 0.00748 | 2.6 |
| `sw_cratio_cs` | 1.47x | 0.477 | 5.68 |
| `i320_sratio_cs` | 1.47x | 0.476 | 6 |
| `s2_zi_asymlaplace` | 1.46x | 0.0304 | 3.71 |
| `gp_regr` | 1.45x | 0.019 | 5.43 |
| `hmm_example` | 1.45x | 0.117 | 4.25 |
| `s2_mixture_theta` | 1.44x | 0.0452 | 4.48 |
| `ch14_m14_2` | 1.44x | 0.0958 | 7.17 |
| `covid19imperial_v3` | 1.44x | 2.04 | 8.67 |
| `s2_zoi_beta` | 1.43x | 0.0358 | 3.81 |
| `Rate_4_model` | 1.43x | 0.00687 | 2.51 |
| `s2_rate` | 1.43x | 0.013 | 3.59 |
| `covid19imperial_v2` | 1.42x | 2.05 | 8.65 |
| `s2_zi_beta` | 1.4x | 0.0353 | 3.76 |
| `Rate_2_model` | 1.4x | 0.0075 | 2.55 |
| `gp_pois_regr` | 1.4x | 0.0175 | 5.66 |
| `Rate_1_model` | 1.4x | 0.00619 | 2.4 |
| `ch11_m_pois` | 1.39x | 0.00688 | 2.53 |
| `i320_gp_expquad` | 1.39x | 0.0649 | 7.36 |
| `sw_exgaussian` | 1.38x | 0.0158 | 3.86 |
| `sw_mixture` | 1.38x | 0.0408 | 4.77 |
| `aalto_bern` | 1.38x | 0.00661 | 2.57 |
| `ch12_m12_6` | 1.38x | 9.64 | 17.4 |
| `aalto_binomb` | 1.37x | 0.0065 | 2.42 |
| `s2_cumulative_probit` | 1.37x | 0.0333 | 3.72 |
| `sw_cumulative_cs` | 1.36x | 0.384 | 5.46 |
| `aalto_binom` | 1.36x | 0.00582 | 2.38 |
| `wells_dist` | 1.36x | 0.128 | 3.17 |
| `aalto_binom2` | 1.36x | 0.00658 | 2.51 |
| `blr` | 1.35x | 0.00869 | 3.07 |
| `ch15_m15_7` | 1.35x | 0.0559 | 7.63 |
| `GLM_Binomial_model` | 1.34x | 0.0123 | 3.28 |
| `ch12_m12_2` | 1.34x | 0.0122 | 3.99 |
| `Rate_3_model` | 1.34x | 0.00682 | 2.42 |
| `i319_negbin_re` | 1.33x | 0.061 | 4.87 |
| `ch14_m14_8nc` | 1.33x | 0.0245 | 6 |
| `Rate_5_model` | 1.32x | 0.00704 | 2.5 |
| `ch12_m12_1` | 1.3x | 0.0129 | 3.24 |
| `sw_beta` | 1.28x | 0.0207 | 3.89 |
| `sw_gp` | 1.27x | 0.109 | 7.41 |
| `ch14_m14_1` | 1.27x | 0.0349 | 6.93 |
| `ch14_m14_6x` | 1.26x | 0.44 | 7.26 |
| `nn_rbm1bJ10` | 1.26x | 0.806 | 6.42 |
| `ch14_m14_6` | 1.26x | 0.442 | 7.26 |
| `ch14_m14_9` | 1.25x | 1.74 | 6.39 |
| `ch14_m14_8` | 1.21x | 0.0237 | 4.78 |
| `s2_custom_vint` | 1.2x | 0.0376 | 3.58 |
| `ch14_m14_10` | 1.2x | 1.91 | 6.52 |
| `ch14_m14_7` | 1.2x | 0.158 | 9.01 |
| `sw_mv_rescor` | 1.19x | 0.0454 | 6.15 |
| `cm_rdm` | 1.19x | 0.306 | 10.9 |
| `sw_categorical` | 1.19x | 0.0184 | 4.36 |
| `s2_frechet` | 1.18x | 0.0213 | 3.95 |
| `sw_weibull` | 1.17x | 0.0209 | 3.92 |
| `sw_bernoulli` | 1.15x | 0.0117 | 3.29 |
| `sw_binomial` | 1.14x | 0.0198 | 3.53 |
| `Survey_model` | 1.13x | 0.303 | 3.31 |
| `sw_sratio` | 1.12x | 0.314 | 4.25 |
| `sw_cratio` | 1.11x | 0.317 | 4.24 |
| `sw_poisson` | 1.11x | 0.0132 | 3.12 |
| `i320_sratio_plain` | 1.1x | 0.322 | 4.4 |
| `s2_multinomial` | 1.09x | 0.06 | 4 |
| `sw_trunc` | 1.08x | 0.043 | 3.56 |
| `s2_hurdle_negbin` | 1.08x | 0.0491 | 3.54 |
| `hmm_gaussian` | 1.08x | 1.48 | 6.27 |
| `ch14_m14_11` | 1.07x | 3.17 | 7.75 |
| `s2_weights_trunc` | 1.07x | 0.0452 | 3.61 |
| `nn_rbm1bJ100` | 1.07x | 2.19e+03 | 2.35e+03 |
| `sw_mono` | 1.07x | 0.0222 | 4.12 |
| `s2_beta_binomial` | 1.06x | 0.0342 | 3.73 |
| `sw_negbinomial` | 1.04x | 0.0184 | 3.42 |
| `hmm_drive_1` | 1.04x | 0.802 | 5.57 |
| `s2_discrete_weibull` | 1.04x | 0.0294 | 3.51 |
| `s2_dirichlet` | 1.04x | 0.0756 | 4.45 |
| `wells_dist100_model` | 1.04x | 0.0945 | 3.31 |
| `aalto_poisson_simple` | 1.03x | 0.0126 | 2.68 |
| `wells_dae_model` | 1.02x | 0.113 | 3.35 |
| `wells_dist100ars_model` | 1.02x | 0.103 | 3.31 |
| `bones_model` | 1.02x | 0.279 | 3.77 |
| `wells_interaction_model` | 1.02x | 0.113 | 3.38 |
| `s2_mo_simo_prior` | 1.01x | 0.0237 | 4.14 |
| `nes_logit_model` | 1.01x | 0.0415 | 3.19 |
| `one_comp_mm_elim_abs` | 1.01x | 2.32 | 6.13 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 0.114 | 3.43 |
| `i319_negbin_fixed` | 1.01x | 0.0522 | 3.45 |
| `i320_pois_trunc_ub` | 1.01x | 0.14 | 3.57 |
| `i319_pois_fixed` | 1.01x | 0.023 | 3.15 |
| `wells_daae_c_model` | 1.01x | 0.116 | 3.49 |
| `s2_wiener` | 1x | 0.139 | 3.81 |
| `cm_ddm` | 1x | 850 | 863 |
| `wells_dae_c_model` | 1x | 0.108 | 3.49 |
| `wells_dae_inter_model` | 1x | 0.118 | 3.46 |
| `cm_betadiscrete` | 0.999x | 10.7 | 15 |
| `diamonds` | 0.995x | 0.191 | 3.58 |
| `iohmm_reg` | 0.99x | 1.91 | 7.88 |
| `sw_cumulative` | 0.982x | 0.121 | 3.87 |
| `s2_cosy` | 0.978x | 0.0506 | 6.77 |
| `s2_ar_cov` | 0.977x | 0.0531 | 7.65 |
| `hmm_drive_0` | 0.973x | 0.778 | 5.5 |
| `i320_pois_trunc_both` | 0.967x | 0.18 | 3.67 |
| `s2_fcor` | 0.962x | 0.0806 | 4.92 |
| `i320_gp_matern32` | 0.92x | 0.169 | 7.35 |
| `garch11` | 0.887x | 0.0491 | 2.97 |
| `cm_invgaussian` | 0.847x | 0.874 | 34.1 |
| `aalto_gpareto` | 0.797x | 0.0139 | 3.08 |
| `arma11` | 0.756x | 0.037 | 3.01 |
| `s2_com_poisson` | 0.212x | 1.73 | 5.02 |

Total sampling is an estimate: each engine's measured setup time + 5,000 × its warm gradient time, averaged over 10 evaluation points. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `hier_2pl` | — | — | — | failed; point 1: density/gradient mismatch: scaled error 7.06e-09 |
| `irt_2pl` | — | — | — | failed; point 1: density/gradient mismatch: scaled error 5.06e-09 |
| `kronecker_gp` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.000113 |
| `logistic_regression_rhs` | — | — | — | failed; point 3: density/gradient mismatch: scaled error 6.9e-09 |
| `s2_gp_by_gr` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5594) |
| `s2_mi_trunc_lb` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 2.99e-07 |
| `sw_cens` | — | — | — | failed; point 0: density/gradient mismatch: scaled error 2.44e-06 |
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

<a id="how-we-measure"></a>

## Appendix: how we measure

Both engines evaluate the same model, data and unconstrained parameter vectors
with the sampling log density (`propto=true`) and the Jacobian adjustment.
There are ten evaluation points per model. Each model also has a gradient
count. Both come from one reference run per model: Stanli's NUTS sampler on the
real data, one chain, seed 1, 1,000 warmup and 1,000 draws, default adaptation,
with the warmup iterations saved. The run records `n_leapfrog__` for every
iteration, and each leapfrog step is one gradient. The gradient count is the
total over warmup and sampling. The file also records the divergent and
maximum-treedepth iterations.

The points are ten iterations of that run, chosen by systematic selection over
the cumulative gradient count of warmup and sampling together. Iteration `i` is
chosen for point `k` (0 to 9) when it is the first whose cumulative count
reaches `(k + 0.5) / 10` of the total. Busy parts of the run, where the sampler
spends its gradients, therefore get more points. The sampler already reports
unconstrained draws, and the points are those values unchanged.

A model falls back to the fixed points if the reference run fails, times out
after 30 minutes, or has a selected point with a non-finite density or
gradient. Point 0 of the fixed points has coordinate `i` equal to
`0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Points 1 to 9 draw each coordinate
uniformly from (-2, 2) with a fixed integer generator seeded from the point
and coordinate. A fallback model uses 5,000 gradients. The vectors, the
counts and the reason for each fallback are in
[`benchmark-points.json.gz`](internal/artifacts/benchmark-points.json.gz).
Both engines read the same vectors. The tables mark fallback models with †.

### Gradient time

Each engine builds its model and evaluates every point once outside the
timed window. It then warms up for at least 20 ms at the first finite point.
At each finite point it calibrates a batch size, then measures batches of
gradients for at least 40 ms. A process's gradient time is the mean of its
per-point times, which is about 0.45 s of measurement per process. Each
measurement is a fresh process, and only one engine runs at a time. Six pairs
alternate the order (Stanli first, then CmdStan first). The reported speedup
is the median of the six within-pair CmdStan/Stanli ratios of those means, so
above 1x means Stanli is faster. The artifacts keep the median absolute
deviation (MAD) of the ratios and of each engine's mean latency; read a small
difference with that dispersion. The drivers share
[`benchmark_timer.hpp`](../tools/benchmark_timer.hpp).

### Numerical gate

Every accepted pair must agree on the log density and
every gradient component at every point within scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. A point that is non-finite in both
engines is dropped and recorded, and the mean covers the remaining points. A
point that is non-finite in only one engine, unequal widths and failed commands
produce no accepted pair, as does a model with no point finite in both engines.
All six pairs must keep the same points. The harness enforces the gate while
it runs. The published records keep each point's timing and its scaled error,
not the density and gradient values. The independent
[three-point corpus replay](../TESTING.md#comparison-with-cmdstan-on-complete-models)
checks correctness separately.

Seven default-run models fail the gate and appear in the incomplete-results
table. Stanli finds no finite point for `s2_invgaussian`. `kronecker_gp`,
`sw_cens` and `s2_mi_trunc_lb` have scaled errors of `1.13e-4`, `2.44e-6` and
`2.99e-7` at point 0. `hier_2pl`, `irt_2pl` and `logistic_regression_rhs` pass
at point 0 but have scaled errors between `5.1e-9` and `7.1e-9` at one of the
random points. The loop-vectorization table also rejects `s2_gp_by_gr` at
`5.33e-8`. Eleven models drop at least one point that is non-finite in both
engines; `sir` keeps only one of its ten points.
[TESTING.md](../TESTING.md#comparison-with-cmdstan-on-complete-models)
lists the numerical exceptions.

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

Each engine's setup time plus the model's reference-run gradient count times
its warm gradient time, the mean over the ten points. The same count is used for
both engines. The benchmark itself runs no sampler. Adaptation, tree building,
generated quantities and CSV output are left out. The count comes from Stanli's
sampler, so it assumes CmdStan's sampler would take about as many gradients on
the same model. `tools/validate_total_estimates.py` checks this: it runs full
sampling in both engines for a sample of models and compares the real sampler
time with the estimate.

A fallback model uses 5,000 gradients. A missing setup component leaves the
estimate blank and keeps the gradient result.

### Loop-vectorization comparison

Stock pinned stanc3 leaves loop vectorization off at `--O1`. The loop-vectorization table turns
that one setting on and runs the resulting compiler with `--O1`, so CmdStan
gets both the O1 optimizations and loop vectorization (this is neither a
vectorization-only ablation nor `--Oexperimental`). The Stanli timings are the
default run's. The CmdStan timings are reused from earlier runs of the same
CmdStan build and toolchain with the same inputs and settings; models those
runs lack are timed again. Each run gates its own Stanli and CmdStan gradients
against each other; the default run's Stanli gradients are not compared with
the reused CmdStan gradients. Because the two sides come from separate
runs, the ratio is CmdStan's mean gradient time divided by Stanli's.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records the pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
and stock and patched MIR for a scalar normal-likelihood loop are kept with the
[evidence](../output/corpus-performance-vectorized/README.md).

<a id="reproduction"></a>

## Appendix: reproduction

| Item | Configuration |
| --- | --- |
| Scope | All 343 application models, `--corpus all` |
| Gradient trials | Six alternating pairs; per process 20 ms warmup, then 40 ms of measurement at each of 10 evaluation points |
| Displayed workload | Setup + reference-run gradient count × mean warm gradient time over the points |
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
cmake --build build-rel --target bench_grad stanli_shared -j 4
python3 tools/gen_benchmark_points.py --lib build-rel/libstanli.dylib --jobs 8
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/default.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 20 --measure-ms 40 --points 10 \
  --gradient-timeout 60 --build-timeout 900
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/vectorized.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 20 --measure-ms 40 --points 10 \
  --gradient-timeout 60 --build-timeout 900 \
  --stanc build/corpus-current/stanc-o1vec-src/_build/default/src/stanc/stanc.exe \
  --stancflags=--O1
```

`--corpus` selects a source collection and `--filter` selects model IDs.
`tools/gen_benchmark_points.py` reruns the reference runs and rewrites the
committed points file (use `libstanli.so` on Linux; `--jobs` and `--timeout`
set the parallelism and the per-model limit). The harness reads the file by
default through `--points-file` and refuses a model whose source or data no
longer matches.
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
averages, logs, raw paired timings and per-point scaled errors. `OUT.tsv` is
derived from these records.

Publish the complete runs, then regenerate the docs and browser catalog:

```sh
python3 tools/publish_corpus_bench.py build/corpus-current/default.tsv \
  output/corpus-performance
V=output/corpus-performance-vectorized
python3 tools/publish_corpus_bench.py build/corpus-current/vectorized.tsv \
  $V/cmdstan-runs/RUN_ID --compiler-evidence build/corpus-current
python3 tools/publish_corpus_bench.py --vectorized $V \
  --stanli-run output/corpus-performance \
  --cmdstan-run $V/cmdstan-runs/RUN_ID [--cmdstan-run ...]
python3 tools/gen_docs.py
python3 tools/gen_web_models.py deps/posteriordb
```

The publisher checks the whole inventory, the raw pairs and the recomputed
estimates, requires every recorded scaled error to be within the gate, and never fills a missing cell from an older run. The one exception
is the loop-vectorization table: when CmdStan, its toolchain and the patched
stanc3 are unchanged, you can skip the vectorized sweep, list the existing runs
under `cmdstan-runs/`, and time only the models they lack with
`--filter MODEL`. A later `--cmdstan-run` takes precedence for the models it
contains. `publish_corpus_bench.py --vectorized` records which run supplied
each model in `combined.json`, and the renderer rechecks inputs, settings,
toolchain and compiler hash.

The [default evidence](../output/corpus-performance/README.md) and
[loop-vectorization evidence](../output/corpus-performance-vectorized/README.md)
directories hold the full inventories, manifests, summary TSVs, raw
timings, command events and checksums. Older runs and protocols are in
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
