# Benchmarks

Stanli runs Stan models without a per-model C++ build. This page compares it
with CmdStan on **<!--gen:benchmark_models-->343<!--/gen--> application models**.

## TL;DR

- The median model's gradient is **<!--gen:corpus_median-->2.16x<!--/gen--> faster** in Stanli than in
  CmdStan. Stanli is as fast or faster on <!--gen:corpus_at_par-->323<!--/gen--> of the
  <!--gen:corpus_n_grad-->338<!--/gen--> models that pass the numerical check.
- In the total-sampling column, which adds setup to the gradients a full
  1,000 warmup + 1,000 draw run takes, the median model is <!--gen:corpus_total_median-->34x<!--/gen--> faster, mostly because
  CmdStan builds C++ for each model. The longer the run, the closer this gets
  to the gradient speedup.
- Regressions, GLMs and IRT models gain the most. Models dominated by dense
  linear algebra, ODE solvers or sequential recurrences land near parity or
  run slower.
- Models that fail the numerical check are listed under each table with the
  reason.
- [Fast mode](#fast-mode) is measured separately and has its own section.

The results tables come first. [How we measure](#how-we-measure) and
[reproduction](#reproduction) are appendices at the end.

<a id="full-corpus"></a>

## Full corpus results

Every application model uses the same settings. Language-conformance fixtures
belong to the [numerical corpus](corpus-status.md) and are not timed here.
CmdStan uses its default compiler settings. The `CmdStan / Stanli gradient` column is
how many times faster Stanli's gradient is; complete results are sorted by it,
fastest first. Total sampling is each engine's setup time plus the warm gradients of a full run, with the gradient count
measured once per model; the benchmark itself runs no sampler ([details](#total-sampling-estimate)).
The last column is how many times faster Stanli's total sampling is.
Incomplete results follow each table.

<!--gen:benchmark_catalog-->
Run `c96e89234d9667fd` (2026-10-20): 343 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 343 of 343 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 34.1x | 18.6k | 1.26 | 45.7 | 36.3x |
| `gpcm_latent_reg_irt` | 12.5x | 69k | 8.06 | 105 | 13.1x |
| `grsm_latent_reg_irt` | 12x | 63.1k | 4.28 | 55.6 | 13x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 47.3k | 1.26 | 16.7 | 13.3x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11x | 68.5k | 1.86 | 23.2 | 12.5x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 50.6k | 2.34 | 26.1 | 11.2x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10x | 81.2k | 3.79 | 40.7 | 10.8x |
| `radon_county_intercept` | 9.88x | 62.8k | 2.84 | 30.7 | 10.8x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.82x | 52.9k | 2.49 | 27 | 10.8x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.72x | 102k | 4.81 | 49.8 | 10.4x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.31x | 77.7k | 5.08 | 50.3 | 9.91x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.29x | 59.7k | 3.05 | 31.1 | 10.2x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.28x | 100k | 6.59 | 63.7 | 9.66x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.01x | 125k | 6.54 | 62.1 | 9.5x |
| `aalto_poisson_hurdle` | 8.78x | 6.92k | 0.0742 | 3.4 | 45.8x |
| `radon_pooled` | 8.48x | 9.33k | 0.397 | 5.98 | 15.1x |
| `dogs` | 7.69x | 20.7k | 0.205 | 5.04 | 24.6x |
| `rats_model` | 7.66x | 54.5k | 0.048 | 3.4 | 71x |
| `s2_custom_vreal` | 7.51x | 10.2k | 0.0119 | 3.5 | 293x |
| `arK` | 7.39x | 59.1k | 0.109 | 3.69 | 33.8x |
| `ch12_m12_3_alt` | 7.13x | 11k | 0.0362 | 2.71 | 74.8x |
| `ch12_m12_3` | 6.97x | 10.8k | 0.0378 | 2.72 | 72x |
| `ch16_m16_1` | 6.84x | 301k | 1.21 | 11.2 | 9.19x |
| `M0_model` | 6.38x | 7.46k | 0.0281 | 2.87 | 102x |
| `Mt_model` | 5.63x | 11k | 0.0505 | 3.73 | 74x |
| `sw_nonlinear` | 5.35x | 10.9k | 0.0138 | 3.91 | 283x |
| `dogs_log` | 5.2x | 12.1k | 0.126 | 3.78 | 30.1x |
| `ctsem_ctsm_400` † | 5.1x | 5k | 20.7 | 189 | 9.15x |
| `ctsem_ctsm` | 5.09x | 1.08M | 430 | 2.27e+03 | 5.27x |
| `ch09_m5_8s` | 4.96x | 1.37M | 0.616 | 5.91 | 9.6x |
| `s2_invgaussian` | 4.8x | 16.3k | 0.0148 | 3.76 | 254x |
| `ch09_m5_8s2` | 4.68x | 1.11M | 0.529 | 5.31 | 10x |
| `ch14_m14_5` | 4.61x | 12k | 0.0346 | 2.98 | 86.2x |
| `s2_cumulative_cauchit` | 4.49x | 20.7k | 0.0302 | 3.73 | 123x |
| `logmesquite` | 4.42x | 56.4k | 0.0342 | 3.27 | 95.6x |
| `logmesquite_logvas` | 4.37x | 92.3k | 0.0495 | 3.59 | 72.5x |
| `ch15_m15_1` | 4.36x | 30.7k | 0.02 | 3.33 | 166x |
| `mesquite` | 4.25x | 160k | 0.079 | 3.41 | 43.1x |
| `logmesquite_logvash` | 4.23x | 93.7k | 0.0453 | 3.49 | 77x |
| `ch15_m15_2` | 4.21x | 32.4k | 0.0267 | 3.51 | 132x |
| `s2_nl_noloop` | 4.16x | 52.6k | 0.0332 | 4.02 | 121x |
| `logmesquite_logva` | 4.1x | 58.7k | 0.0243 | 3.23 | 133x |
| `s2_logistic_normal` | 4.03x | 13.6k | 0.0853 | 6.55 | 76.8x |
| `s2_car_icar` | 4.03x | 1.7M | 0.722 | 7.29 | 10.1x |
| `nes` | 3.98x | 90.8k | 1.68 | 9.88 | 5.86x |
| `election88_full` | 3.89x | 429k | 104 | 411 | 3.96x |
| `ch14_m14_4` | 3.84x | 9.05k | 0.0218 | 2.86 | 131x |
| `ch14_m14_4x` | 3.83x | 9.05k | 0.0218 | 2.86 | 132x |
| `sw_me` | 3.81x | 57.9k | 0.0406 | 4.25 | 105x |
| `logmesquite_logvolume` | 3.8x | 12.4k | 0.00914 | 3.02 | 331x |
| `Mth_model` | 3.8x | 33.9k | 0.899 | 7.7 | 8.56x |
| `sw_spline_s` | 3.73x | 37.6k | 0.0278 | 4.52 | 162x |
| `s2_s_cc` | 3.73x | 30.5k | 0.0217 | 4.36 | 201x |
| `sw_dist_sigma` | 3.72x | 10.7k | 0.017 | 3.87 | 228x |
| `s2_nlf` | 3.72x | 9.42k | 0.0165 | 3.99 | 242x |
| `s2_me2_nomecor` | 3.68x | 50.4k | 0.0556 | 4.58 | 82.3x |
| `aalto_grp_aov` | 3.62x | 12.5k | 0.00844 | 3.13 | 371x |
| `s2_dist_sigma_re` | 3.62x | 29.4k | 0.0314 | 4.96 | 158x |
| `kilpisjarvi` | 3.6x | 1.12M | 0.227 | 3.61 | 15.9x |
| `kidscore_mom_work` | 3.58x | 40.5k | 0.113 | 3.3 | 29.2x |
| `sw_weights` | 3.57x | 9.98k | 0.0147 | 3.54 | 241x |
| `ch09_m9_1` | 3.57x | 11.3k | 0.0149 | 3.14 | 211x |
| `ch09_m9_1_chains4` | 3.57x | 11.3k | 0.0159 | 3.14 | 198x |
| `kidscore_interaction` | 3.55x | 178k | 0.478 | 4.71 | 9.85x |
| `kidscore_interaction_c2` | 3.53x | 17.7k | 0.0545 | 3.16 | 58x |
| `kidscore_interaction_z` | 3.52x | 24k | 0.0707 | 3.32 | 47x |
| `kidscore_interaction_c` | 3.5x | 15.2k | 0.047 | 3.18 | 67.5x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.5x | 1.03M | 6.24 | 26.6 | 4.26x |
| `sw_se` | 3.48x | 7.2k | 0.0114 | 3.52 | 310x |
| `s2_shifted_lognormal` | 3.47x | 15.1k | 0.0163 | 3.94 | 241x |
| `s2_gp_approx` | 3.46x | 34k | 0.0332 | 5.29 | 159x |
| `s2_cox_cens` | 3.45x | 22.1k | 0.0317 | 5.13 | 162x |
| `logearn_interaction` | 3.42x | 322k | 2.55 | 11.6 | 4.56x |
| `logearn_height_male` | 3.42x | 189k | 1.06 | 6.52 | 6.15x |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.39x | 17.3k | 0.00896 | 3.29 | 367x |
| `s2_mm_weights` | 3.38x | 27.8k | 0.0279 | 4.73 | 169x |
| `GLMM1_model` | 3.38x | 32.8k | 0.332 | 4.32 | 13x |
| `s2_mm` | 3.37x | 26.6k | 0.0283 | 4.72 | 167x |
| `logearn_interaction_z` | 3.37x | 22k | 0.182 | 3.71 | 20.4x |
| `ch16_m16_4` | 3.36x | 35.3k | 0.0636 | 3.1 | 48.7x |
| `logearn_logheight_male` | 3.35x | 715k | 4.04 | 16.4 | 4.07x |
| `sw_lognormal` | 3.32x | 8.28k | 0.0115 | 3.71 | 324x |
| `s2_car` | 3.28x | 30.6k | 0.0311 | 4.43 | 142x |
| `ch15_m15_9` | 3.27x | 11.3k | 0.0357 | 3.1 | 86.8x |
| `s2_car_esicar` | 3.27x | 28.9k | 0.0286 | 4.55 | 159x |
| `s2_s_by` | 3.26x | 70.7k | 0.12 | 6.2 | 51.6x |
| `radon_county` | 3.26x | 48.8k | 1.29 | 7.27 | 5.65x |
| `ch15_m15_5` | 3.25x | 21.5k | 0.0165 | 3.3 | 200x |
| `sw_spline_t2` | 3.25x | 38.1k | 0.0468 | 5.05 | 108x |
| `kidscore_momhsiq` | 3.21x | 90.5k | 0.188 | 3.54 | 18.8x |
| `s2_gp_by_approx` | 3.2x | 37.2k | 0.055 | 6.13 | 111x |
| `ch15_m15_8` | 3.19x | 10.7k | 0.0333 | 2.94 | 88.3x |
| `s2_gr_by` | 3.13x | 33.2k | 0.0281 | 4.99 | 178x |
| `s2_t2_by` | 3.13x | 36.4k | 0.0735 | 5.99 | 81.5x |
| `kidscore_momiq` | 3.12x | 60.1k | 0.0905 | 3.13 | 34.6x |
| `cm_exwald` | 3.11x | 68k | 2.99 | 13.4 | 4.48x |
| `s2_cox` | 3.1x | 30.6k | 0.0336 | 4.25 | 127x |
| `aalto_grp_prior_mean` | 3.06x | 19.3k | 0.00989 | 3.21 | 324x |
| `i319_gauss_re` | 3.03x | 41.3k | 0.0739 | 4.76 | 64.4x |
| `sw_acat` | 3.02x | 12.2k | 0.602 | 5.66 | 9.41x |
| `sw_re_gauss` | 2.97x | 29.1k | 0.0242 | 4.62 | 191x |
| `ch15_m15_6` | 2.96x | 15.6k | 0.0114 | 3.02 | 266x |
| `s2_hurdle_cumulative` | 2.88x | 11.2k | 0.0418 | 4.29 | 103x |
| `sw_student` | 2.88x | 12.7k | 0.0161 | 3.89 | 242x |
| `logearn_height` | 2.87x | 138k | 0.531 | 4.35 | 8.19x |
| `s2_cumulative_cloglog` | 2.86x | 20.3k | 0.0341 | 3.73 | 109x |
| `earn_height` | 2.85x | 150k | 0.575 | 4.42 | 7.69x |
| `log10earn_height` | 2.85x | 143k | 0.542 | 4.36 | 8.05x |
| `bym2_offset_only` | 2.85x | 202k | 7.66 | 26.1 | 3.41x |
| `s2_mi_lognormal` | 2.85x | 14.8k | 0.021 | 4.62 | 220x |
| `sesame_one_pred_a` | 2.84x | 15.5k | 0.0188 | 2.84 | 151x |
| `s2_gr_student` | 2.84x | 52.2k | 0.0459 | 5.27 | 115x |
| `aalto_lin_std` | 2.83x | 11k | 0.00883 | 3.34 | 379x |
| `blr` | 2.83x | 32.4k | 0.0185 | 3.21 | 173x |
| `Mtbh_model` | 2.83x | 34.3k | 0.571 | 6.56 | 11.5x |
| `pilots` | 2.82x | 551k | 0.192 | 3.92 | 20.5x |
| `eight_schools_centered` | 2.82x | 32.4k | 0.00986 | 2.99 | 303x |
| `aalto_lin` | 2.81x | 18.2k | 0.0102 | 3.12 | 305x |
| `dogs_nonhierarchical` | 2.79x | 56.4k | 0.889 | 9.3 | 10.5x |
| `aalto_grp_prior_mean_var` | 2.78x | 59.3k | 0.0234 | 3.98 | 170x |
| `GLMM_Poisson_model` | 2.77x | 75.6k | 0.0533 | 4.01 | 75.2x |
| `sw_hurdle_lognormal` | 2.76x | 12.5k | 0.026 | 3.6 | 139x |
| `s2_mmc` | 2.72x | 35.4k | 0.0636 | 7.09 | 112x |
| `ch13_m13_4nc` | 2.72x | 75.5k | 0.667 | 5.11 | 7.65x |
| `GLM_Poisson_model` | 2.7x | 29.5k | 0.0192 | 3.37 | 175x |
| `kidscore_momhs` | 2.67x | 24.8k | 0.0421 | 2.97 | 70.5x |
| `sw_gamma` | 2.66x | 9.45k | 0.0149 | 3.78 | 253x |
| `sw_hurdle_pois` | 2.6x | 10.4k | 0.0285 | 3.44 | 121x |
| `accel_gp` | 2.59x | 1.5M | 5.32 | 19.2 | 3.61x |
| `ch12_m12_7` | 2.59x | 48.6k | 49.7 | 132 | 2.66x |
| `cm_geg` | 2.59x | 54k | 1.37 | 7.4 | 5.39x |
| `hierarchical_gp` | 2.58x | 352k | 6.06 | 24.7 | 4.07x |
| `cm_exgaussian` | 2.58x | 14k | 0.222 | 4.2 | 18.9x |
| `ch12_m12_5` | 2.58x | 37.4k | 38.1 | 103 | 2.69x |
| `losscurve_sislob` | 2.55x | 21.3k | 0.0327 | 4.36 | 133x |
| `s2_gev` | 2.55x | 12.2k | 0.0358 | 3.83 | 107x |
| `sw_acat_cs` | 2.52x | 18.3k | 1.29 | 8.22 | 6.38x |
| `s2_me2` | 2.52x | 71.6k | 0.132 | 7.01 | 53.2x |
| `accel_splines` | 2.52x | 1.86M | 7.05 | 22.1 | 3.14x |
| `s2_sar` | 2.5x | 10.9k | 0.0496 | 4.17 | 84x |
| `s2_sar_error` | 2.49x | 11.4k | 0.0517 | 4.25 | 82.1x |
| `ch11_m11_10` | 2.48x | 10.7k | 0.00966 | 3.28 | 340x |
| `ldaK5` † | 2.48x | 5k | 12.8 | 34 | 2.67x |
| `ch14_m14_3` | 2.45x | 65.2k | 0.594 | 7.58 | 12.8x |
| `seeds_centered_model` | 2.4x | 41.5k | 0.036 | 3.94 | 109x |
| `i319_pois_re2` | 2.39x | 173k | 0.823 | 6.72 | 8.17x |
| `s2_rate` | 2.37x | 7.8k | 0.0128 | 3.62 | 282x |
| `dogs_hierarchical` | 2.36x | 9.37k | 0.174 | 3.15 | 18.1x |
| `Mh_model` | 2.35x | 53.8k | 0.912 | 5.49 | 6.02x |
| `ch15_m15_3` | 2.33x | 7.74k | 0.109 | 2.9 | 26.6x |
| `normal_mixture` | 2.32x | 7.96k | 0.306 | 3.29 | 10.7x |
| `sw_re_bern` | 2.32x | 27.4k | 0.0288 | 4.71 | 164x |
| `ch15_m15_4` | 2.31x | 8.81k | 0.101 | 2.88 | 28.5x |
| `ch09_m9_5` | 2.31x | 398k | 0.0601 | 2.7 | 44.9x |
| `ch09_m9_3` | 2.3x | 10.8k | 0.00711 | 2.57 | 361x |
| `extra_hurdle_poisson` | 2.3x | 8.4k | 0.00849 | 2.59 | 306x |
| `s2_mv_shared_re` | 2.29x | 36.8k | 0.0672 | 7.2 | 107x |
| `i320_mi_nhanes` | 2.29x | 312k | 0.274 | 5.03 | 18.4x |
| `ch09_m9_2` | 2.29x | 46.7k | 0.00953 | 2.54 | 267x |
| `ch09_m9_4` | 2.29x | 1.55M | 0.214 | 3.08 | 14.4x |
| `sw_skewnormal` | 2.27x | 15.5k | 0.0252 | 4.01 | 159x |
| `ch11_m11_5` | 2.27x | 29.3k | 0.202 | 3.57 | 17.7x |
| `aalto_lin_std_t` | 2.24x | 13k | 0.0114 | 3.54 | 310x |
| `sw_vonmises` | 2.21x | 730k | 0.516 | 4.81 | 9.32x |
| `sw_re_slope` | 2.21x | 53.4k | 0.0686 | 6.88 | 100x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.2x | 44.4k | 2.12 | 7.73 | 3.64x |
| `dugongs_model` | 2.18x | 33.4k | 0.023 | 3.06 | 133x |
| `ch13_m13_4` | 2.16x | 34.4k | 0.25 | 4.1 | 16.4x |
| `ldaK2` | 2.16x | 19.8k | 1.01 | 5.87 | 5.78x |
| `2pl_latent_reg_irt` | 2.16x | 51.2k | 3.4 | 12.9 | 3.79x |
| `hier_2pl` | 2.15x | 59.4k | 11.9 | 32.6 | 2.73x |
| `ch13_m13_4b` | 2.15x | 34.4k | 0.246 | 3.87 | 15.7x |
| `lsat_model` | 2.15x | 40.8k | 1.84 | 7.68 | 4.18x |
| `ch13_m13_7` | 2.14x | 19.9k | 0.00725 | 2.34 | 323x |
| `low_dim_gauss_mix` | 2.14x | 14k | 0.691 | 4.68 | 6.78x |
| `ch13_m13_6` | 2.12x | 36.1k | 0.265 | 3.93 | 14.9x |
| `sw_re_pois` | 2.11x | 30k | 0.0344 | 4.54 | 132x |
| `sw_zi_poisson` | 2.11x | 11.5k | 0.0254 | 3.41 | 134x |
| `seeds_model` | 2.08x | 38.4k | 0.0331 | 3.7 | 112x |
| `cm_bisa` | 2.07x | 27.9k | 0.646 | 5.34 | 8.27x |
| `s2_index_mi` | 2.07x | 18.6k | 0.0264 | 4.24 | 160x |
| `multi_occupancy` | 2.05x | 98.9k | 3.03 | 12.2 | 4.03x |
| `ch09_mp` | 2.03x | 38.9k | 0.00741 | 2.35 | 318x |
| `cm_rdm` | 2.03x | 67.7k | 8.37 | 21.1 | 2.52x |
| `sw_exgaussian` | 2x | 25.1k | 0.0315 | 3.96 | 126x |
| `seeds_stanified_model` | 2x | 42.2k | 0.0354 | 3.65 | 103x |
| `ch13_m13_7nc` | 2x | 7.88k | 0.00623 | 2.43 | 389x |
| `cm_lnr` | 1.97x | 41.9k | 4.82 | 13.6 | 2.83x |
| `prophet` | 1.97x | 1.66M | 61.9 | 127 | 2.05x |
| `ch11_m11_4` | 1.97x | 16.5k | 0.109 | 3.23 | 29.8x |
| `cm_lognormal` | 1.97x | 68k | 2.69 | 9.49 | 3.52x |
| `normal_mixture_k` | 1.97x | 243k | 48.1 | 97.8 | 2.03x |
| `i319_pois_re` | 1.95x | 192k | 0.705 | 5.84 | 8.29x |
| `ch13_m13_5` | 1.95x | 18.9k | 0.125 | 3.49 | 28x |
| `ch11_m11_6` | 1.92x | 16.7k | 0.0152 | 3.08 | 202x |
| `ch11_m11_8` | 1.91x | 50.9k | 0.0219 | 2.99 | 137x |
| `ch13_m13_2` | 1.89x | 22.9k | 0.024 | 3.3 | 138x |
| `soil_incubation` | 1.88x | 203k | 4.97 | 12.8 | 2.57x |
| `ch13_m13_1` | 1.87x | 18.5k | 0.0201 | 3.16 | 157x |
| `sw_hurdle_gamma` | 1.85x | 13.3k | 0.0383 | 3.61 | 94.2x |
| `cm_loggamma` | 1.84x | 226k | 6.78 | 16.6 | 2.45x |
| `ch13_m13_3` | 1.84x | 27.8k | 0.0302 | 3.23 | 107x |
| `s2_cens_interval` | 1.84x | 8.94k | 0.0243 | 3.78 | 155x |
| `sw_arma` | 1.83x | 42.2k | 0.105 | 4.53 | 43.1x |
| `cm_logweibull` | 1.8x | 90.4k | 1.87 | 7.37 | 3.94x |
| `sw_ma` | 1.8x | 11.1k | 0.0288 | 4.29 | 149x |
| `ch11_m11_11` | 1.79x | 143k | 0.0605 | 3.71 | 61.2x |
| `sw_ar` | 1.79x | 10.4k | 0.0273 | 4.29 | 157x |
| `sw_mi` | 1.78x | 15.1k | 0.0198 | 3.88 | 196x |
| `lotka_volterra` | 1.77x | 75.1k | 2.58 | 8.91 | 3.46x |
| `sw_beta` | 1.76x | 8.99k | 0.0247 | 3.94 | 160x |
| `s2_threading` | 1.75x | 11.8k | 0.0119 | 3.41 | 286x |
| `sw_zi_binomial` | 1.73x | 10.2k | 0.0344 | 3.45 | 100x |
| `cm_choco` | 1.73x | 15.1k | 0.69 | 5.64 | 8.17x |
| `cm_logstudent` | 1.73x | 2.02M | 30.6 | 57.7 | 1.88x |
| `cm_lba1` | 1.71x | 145k | 8 | 17.9 | 2.24x |
| `s2_mv_subset` | 1.68x | 12.1k | 0.0164 | 3.5 | 214x |
| `wells_dist` | 1.67x | 26.5k | 0.664 | 4.05 | 6.1x |
| `cm_weibull` | 1.67x | 2.03M | 37.5 | 67.1 | 1.79x |
| `cm_gamma` | 1.67x | 2.01M | 41.9 | 73.7 | 1.76x |
| `sw_asymlaplace` | 1.66x | 23.4k | 0.0581 | 3.63 | 62.5x |
| `cm_betagate` | 1.62x | 11.8k | 0.419 | 4.64 | 11.1x |
| `s2_zoi_beta` | 1.61x | 12.1k | 0.0612 | 3.76 | 61.5x |
| `surgical_model` | 1.61x | 22.2k | 0.0162 | 3.47 | 214x |
| `sw_gaussian` | 1.6x | 10.9k | 0.0111 | 3.32 | 299x |
| `s2_zi_beta` | 1.6x | 10.2k | 0.0519 | 3.71 | 71.4x |
| `ch11_m11_7` | 1.6x | 7.03k | 0.00827 | 2.91 | 352x |
| `GLM_Binomial_model` | 1.58x | 13.3k | 0.0189 | 3.39 | 179x |
| `s2_mixture_theta` | 1.58x | 80.4k | 0.454 | 5.24 | 11.5x |
| `sw_mv_norescor` | 1.58x | 11.6k | 0.0164 | 3.48 | 213x |
| `cm_invweibull` | 1.57x | 87.1k | 2.02 | 7.17 | 3.55x |
| `sir` | 1.57x | 127k | 20.8 | 36.7 | 1.77x |
| `cm_lnr_bench` | 1.57x | 40.1k | 10.7 | 21.2 | 1.97x |
| `gp_pois_regr` | 1.53x | 396k | 0.743 | 6.91 | 9.3x |
| `s2_frechet` | 1.52x | 33.2k | 0.0737 | 4.08 | 55.3x |
| `s2_categorical_re` | 1.51x | 34.6k | 0.182 | 5.69 | 31.3x |
| `cm_invgamma` | 1.51x | 79k | 1.82 | 6.76 | 3.71x |
| `Rate_4_model` | 1.5x | 8.3k | 0.00752 | 2.56 | 341x |
| `sw_re_negbin` | 1.5x | 30.3k | 0.0661 | 4.82 | 72.9x |
| `s2_unstr` | 1.49x | 46.4k | 0.224 | 7.77 | 34.7x |
| `cm_lba2` | 1.48x | 140k | 13.6 | 24.8 | 1.82x |
| `sw_weibull` | 1.48x | 10.3k | 0.0291 | 3.92 | 135x |
| `s2_gp_by_gr` | 1.48x | 59.6k | 0.431 | 9.7 | 22.5x |
| `sw_zi_negbin` | 1.48x | 20.5k | 0.073 | 3.56 | 48.7x |
| `gp_regr` | 1.47x | 8.82k | 0.0277 | 5.49 | 198x |
| `covid19imperial_v3` | 1.47x | 473k | 118 | 179 | 1.52x |
| `aalto_binom` | 1.46x | 5.11k | 0.00635 | 2.42 | 382x |
| `Mb_model` | 1.45x | 11k | 0.434 | 3.94 | 9.08x |
| `covid19imperial_v2` | 1.44x | 473k | 118 | 176 | 1.49x |
| `ch11_m11_9` | 1.44x | 5.08k | 0.00718 | 2.61 | 364x |
| `s2_com_poisson` | 1.44x | 42.5k | 0.886 | 4.89 | 5.51x |
| `ch14_m14_2` | 1.43x | 135k | 2.26 | 10.6 | 4.69x |
| `Rate_1_model` | 1.43x | 4.69k | 0.00804 | 2.42 | 300x |
| `hmm_example` | 1.42x | 25.1k | 0.509 | 4.78 | 9.4x |
| `s2_zi_asymlaplace` | 1.42x | 28.3k | 0.0969 | 3.73 | 38.5x |
| `aalto_bern` | 1.42x | 5.11k | 0.00651 | 2.58 | 397x |
| `ch15_m15_7` | 1.41x | 28k | 0.26 | 7.5 | 28.8x |
| `i320_gp_expquad` | 1.4x | 52.4k | 0.571 | 7.83 | 13.7x |
| `ch12_m12_6` | 1.4x | 111k | 219 | 311 | 1.42x |
| `Rate_2_model` | 1.39x | 7.31k | 0.00816 | 2.6 | 318x |
| `aalto_binom2` | 1.39x | 7.7k | 0.00776 | 2.52 | 325x |
| `Rate_5_model` | 1.38x | 5.33k | 0.00669 | 2.52 | 377x |
| `Rate_3_model` | 1.37x | 5.2k | 0.00755 | 2.44 | 324x |
| `ch11_m_pois` | 1.36x | 6.85k | 0.0072 | 2.55 | 354x |
| `ch14_m14_8nc` | 1.35x | 263k | 0.731 | 6.94 | 9.49x |
| `sw_binomial` | 1.35x | 7.52k | 0.0225 | 3.57 | 158x |
| `sw_mono` | 1.34x | 20.4k | 0.0438 | 4.02 | 91.9x |
| `ch12_m12_1` | 1.33x | 11.8k | 0.0198 | 3.27 | 165x |
| `ch12_m12_2` | 1.33x | 64.2k | 0.0635 | 4.24 | 66.9x |
| `i319_negbin_re` | 1.32x | 66.7k | 0.656 | 5.61 | 8.56x |
| `s2_mo_simo_prior` | 1.32x | 21.3k | 0.0473 | 4.03 | 85.1x |
| `aalto_binomb` | 1.31x | 5.61k | 0.00687 | 2.46 | 357x |
| `ch14_m14_1` | 1.3x | 37.6k | 0.206 | 7.55 | 36.7x |
| `s2_beta_binomial` | 1.29x | 10.4k | 0.0532 | 3.76 | 70.7x |
| `sw_mixture` | 1.28x | 303k | 1.6 | 6.82 | 4.27x |
| `sw_sratio` | 1.28x | 10.4k | 0.626 | 4.41 | 7.05x |
| `sw_gp` | 1.27x | 47.3k | 0.921 | 8.22 | 8.92x |
| `ch14_m14_9` | 1.26x | 12.5k | 4.33 | 9.77 | 2.25x |
| `nn_rbm1bJ10` | 1.26x | 2.03M | 326 | 416 | 1.27x |
| `ch14_m14_6` | 1.25x | 21.6k | 1.9 | 9.07 | 4.77x |
| `ch14_m14_6x` | 1.23x | 21.6k | 1.91 | 9.06 | 4.75x |
| `ch14_m14_10` | 1.22x | 11.2k | 4.21 | 9.42 | 2.23x |
| `i320_sratio_plain` | 1.21x | 15.4k | 0.929 | 4.91 | 5.28x |
| `sw_cratio` | 1.21x | 10.4k | 0.631 | 4.36 | 6.92x |
| `ch14_m14_8` | 1.21x | 204k | 0.576 | 5.42 | 9.4x |
| `ch14_m14_7` | 1.2x | 143k | 4.03 | 13.7 | 3.39x |
| `sw_mv_rescor` | 1.19x | 13.3k | 0.0901 | 6.28 | 69.7x |
| `sw_bernoulli` | 1.19x | 8.63k | 0.0117 | 3.28 | 280x |
| `s2_cumulative_probit` | 1.18x | 13.7k | 0.0668 | 3.77 | 56.4x |
| `s2_custom_vint` | 1.18x | 10.5k | 0.0608 | 3.61 | 59.4x |
| `i320_sratio_cs` | 1.15x | 20.4k | 1.83 | 7.08 | 3.87x |
| `sw_cratio_cs` | 1.15x | 13.7k | 1.25 | 6.16 | 4.93x |
| `sw_poisson` | 1.14x | 9.85k | 0.014 | 3.14 | 224x |
| `s2_discrete_weibull` | 1.13x | 31.8k | 0.12 | 3.61 | 30.2x |
| `s2_multinomial` | 1.13x | 12.9k | 0.129 | 4.13 | 32.1x |
| `Survey_model` | 1.13x | 11.1k | 0.65 | 3.76 | 5.78x |
| `sw_categorical` | 1.12x | 11k | 0.022 | 4.32 | 196x |
| `s2_hurdle_negbin` | 1.09x | 15.2k | 0.111 | 3.58 | 32.3x |
| `ch14_m14_11` | 1.09x | 12.7k | 7.87 | 12.7 | 1.62x |
| `sw_negbinomial` | 1.08x | 1.49M | 2.07 | 5.62 | 2.71x |
| `hmm_gaussian` | 1.08x | 63.9k | 17.2 | 23.3 | 1.35x |
| `s2_weights_trunc` | 1.08x | 25.1k | 0.172 | 3.8 | 22.1x |
| `nn_rbm1bJ100` † | 1.07x | 5k | 2.2e+03 | 2.37e+03 | 1.08x |
| `sw_trunc` | 1.07x | 27.2k | 0.177 | 3.76 | 21.3x |
| `hmm_drive_1` | 1.05x | 39.6k | 5.95 | 10.9 | 1.83x |
| `s2_dirichlet` | 1.05x | 12.6k | 0.17 | 4.55 | 26.7x |
| `bones_model` | 1.04x | 14.7k | 0.776 | 4.3 | 5.54x |
| `arma11` | 1.03x | 11.2k | 0.0687 | 3.06 | 44.6x |
| `iohmm_reg` | 1.03x | 462k | 156 | 166 | 1.06x |
| `i319_pois_fixed` | 1.03x | 24k | 0.0738 | 3.18 | 43.1x |
| `wells_dist100_model` | 1.03x | 11.5k | 0.206 | 3.38 | 16.4x |
| `garch11` | 1.03x | 23.3k | 0.201 | 3.17 | 15.7x |
| `nes_logit_model` | 1.02x | 17.8k | 0.127 | 3.24 | 25.5x |
| `aalto_poisson_simple` | 1.02x | 5.02k | 0.0128 | 2.67 | 209x |
| `wells_daae_c_model` | 1.02x | 17.6k | 0.375 | 3.76 | 10x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.325 | 3.7 | 11.4x |
| `diamonds` | 1.01x | 1.51M | 49.6 | 54 | 1.09x |
| `wells_dae_inter_model` | 1.01x | 12.8k | 0.284 | 3.61 | 12.7x |
| `wells_dae_model` | 1.01x | 24.2k | 0.502 | 3.71 | 7.38x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 11.2k | 0.235 | 3.55 | 15.1x |
| `i319_negbin_fixed` | 1.01x | 19.4k | 0.187 | 3.56 | 19x |
| `wells_dist100ars_model` | 1.01x | 19.1k | 0.367 | 3.58 | 9.74x |
| `cm_ddm` † | 1x | 5k | 855 | 863 | 1.01x |
| `s2_wiener` | 0.999x | 14.7k | 0.37 | 4.03 | 10.9x |
| `cm_betadiscrete` | 0.998x | 9.91k | 22.9 | 27.1 | 1.18x |
| `i320_pois_trunc_ub` | 0.997x | 21.9k | 0.58 | 4.09 | 7.05x |
| `i320_pois_trunc_both` | 0.989x | 21.9k | 0.731 | 4.26 | 5.83x |
| `sw_cumulative` | 0.989x | 9.89k | 0.226 | 4.01 | 17.8x |
| `wells_interaction_model` | 0.987x | 32.1k | 0.668 | 3.91 | 5.84x |
| `sw_cumulative_cs` | 0.979x | 22.3k | 1.45 | 6.13 | 4.22x |
| `s2_cosy` | 0.971x | 14.7k | 0.102 | 6.57 | 64.3x |
| `s2_ar_cov` | 0.966x | 11.2k | 0.0896 | 7.26 | 81x |
| `hmm_drive_0` | 0.957x | 24.5k | 3.7 | 8.34 | 2.26x |
| `one_comp_mm_elim_abs` | 0.957x | 18.9k | 8.84 | 12 | 1.35x |
| `s2_fcor` | 0.955x | 9.87k | 0.148 | 5.04 | 34.1x |
| `i320_gp_matern32` | 0.923x | 133k | 4.23 | 10.8 | 2.56x |
| `aalto_gpareto` | 0.76x | 9.37k | 0.0157 | 2.91 | 185x |
| `cm_invgaussian` † | 0.76x | 5k | 0.872 | 5.26 | 6.04x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling | Reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `irt_2pl` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 4.71e-08 |
| `kronecker_gp` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.0723 |
| `logistic_regression_rhs` | — | — | — | — | — | failed; point 2: density/gradient mismatch: scaled error 3.79e-08 |
| `s2_mi_trunc_lb` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 6.93e-05 |
| `sw_cens` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 1.02e-06 |
<!--/gen-->

### CmdStan with stanc3 loop vectorization

This table repeats the comparison with stanc3's loop vectorization turned on
for CmdStan at `--O1`. The Stanli timings are the default run's.
[How this table is built](#loop-vectorization-comparison) is in the appendix.

<!--gen:benchmark_vectorized_catalog-->
Stanli timings are from the default run `c96e89234d9667fd`. The CmdStan timings with loop vectorization are reused from run `0b1edfdec4d6010b` (2026-10-20). Each ratio is CmdStan's median gradient time divided by Stanli's, each averaged over 10 evaluation points.

CmdStan stanc and build times for 6 of 343 models were reused from the build cache and not remeasured in the runs that report them.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 34.1x | 18.6k | 1.26 | 45.5 | 36.1x |
| `gpcm_latent_reg_irt` | 12.9x | 69k | 8.06 | 108 | 13.4x |
| `grsm_latent_reg_irt` | 11.9x | 63.1k | 4.28 | 56 | 13.1x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.4x | 47.3k | 1.26 | 17 | 13.5x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 68.5k | 1.86 | 23.2 | 12.4x |
| `aalto_poisson_hurdle` | 8.83x | 6.92k | 0.0742 | 3.38 | 45.6x |
| `radon_county_intercept` | 8.24x | 62.8k | 2.84 | 25.9 | 9.12x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8.02x | 81.2k | 3.79 | 33.2 | 8.78x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.02x | 50.6k | 2.34 | 21.4 | 9.14x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 7.93x | 52.9k | 2.49 | 22.4 | 9x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.86x | 102k | 4.81 | 40.7 | 8.46x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.63x | 59.7k | 3.05 | 26 | 8.54x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.48x | 77.7k | 5.08 | 40.6 | 8x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.4x | 100k | 6.59 | 51.5 | 7.82x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.33x | 125k | 6.54 | 51 | 7.8x |
| `s2_custom_vreal` | 7.17x | 10.2k | 0.0119 | 3.46 | 290x |
| `ch12_m12_3_alt` | 7x | 11k | 0.0362 | 2.69 | 74.2x |
| `ch16_m16_1` | 6.86x | 301k | 1.21 | 11.3 | 9.27x |
| `rats_model` | 6.76x | 54.5k | 0.048 | 3.35 | 69.8x |
| `ch12_m12_3` | 6.74x | 10.8k | 0.0378 | 2.7 | 71.4x |
| `M0_model` | 6.34x | 7.46k | 0.0281 | 2.81 | 100x |
| `ctsem_ctsm` | 5.57x | 1.08M | 430 | 2.83e+03 | 6.58x |
| `ctsem_ctsm_400` † | 5.5x | 5k | 20.7 | 549 | 26.5x |
| `arK` | 5.09x | 59.1k | 0.109 | 3.36 | 30.8x |
| `Mt_model` | 5.03x | 11k | 0.0505 | 3.67 | 72.8x |
| `s2_invgaussian` | 4.72x | 16.3k | 0.0148 | 3.7 | 250x |
| `Mth_model` | 4.08x | 33.9k | 0.899 | 7.9 | 8.79x |
| `s2_car_icar` | 4.06x | 1.7M | 0.722 | 7.29 | 10.1x |
| `s2_cumulative_cauchit` | 3.87x | 20.7k | 0.0302 | 3.83 | 127x |
| `s2_logistic_normal` | 3.66x | 13.6k | 0.0853 | 6.92 | 81.1x |
| `dogs_log` | 3.53x | 12.1k | 0.126 | 4.16 | 33.1x |
| `sw_weights` | 3.52x | 9.98k | 0.0147 | 3.48 | 237x |
| `s2_mm_weights` | 3.43x | 27.8k | 0.0279 | 4.76 | 170x |
| `s2_mm` | 3.41x | 26.6k | 0.0283 | 4.75 | 168x |
| `dogs` | 3.41x | 20.7k | 0.205 | 4.78 | 23.3x |
| `s2_cox_cens` | 3.4x | 22.1k | 0.0317 | 5.04 | 159x |
| `sw_me` | 3.34x | 57.9k | 0.0406 | 4.36 | 107x |
| `s2_gp_by_approx` | 3.33x | 37.2k | 0.055 | 6.64 | 121x |
| `aalto_grp_aov` | 3.29x | 12.5k | 0.00844 | 3.05 | 361x |
| `GLMM1_model` | 3.29x | 32.8k | 0.332 | 4.31 | 13x |
| `s2_me2_nomecor` | 3.27x | 50.4k | 0.0556 | 4.66 | 83.8x |
| `cm_exgaussian` | 3.26x | 14k | 0.222 | 5.02 | 22.6x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.23x | 1.03M | 6.24 | 25 | 4x |
| `s2_car` | 3.22x | 30.6k | 0.0311 | 4.75 | 153x |
| `cm_exwald` | 3.21x | 68k | 2.99 | 15.3 | 5.11x |
| `i319_gauss_re` | 3.18x | 41.3k | 0.0739 | 4.78 | 64.7x |
| `s2_car_esicar` | 3.17x | 28.9k | 0.0286 | 4.8 | 168x |
| `s2_gr_by` | 3.15x | 33.2k | 0.0281 | 4.98 | 177x |
| `logmesquite_logva` | 3.13x | 58.7k | 0.0243 | 3.37 | 139x |
| `logmesquite_logvas` | 3.12x | 92.3k | 0.0495 | 3.77 | 76.1x |
| `logmesquite` | 3.12x | 56.4k | 0.0342 | 3.48 | 102x |
| `s2_dist_sigma_re` | 3.09x | 29.4k | 0.0314 | 5.01 | 160x |
| `ch09_m9_1` | 3.09x | 11.3k | 0.0149 | 3.32 | 223x |
| `mesquite` | 3.09x | 160k | 0.079 | 3.58 | 45.2x |
| `logmesquite_logvash` | 3.08x | 93.7k | 0.0453 | 3.66 | 80.6x |
| `sw_re_gauss` | 3.05x | 29.1k | 0.0242 | 4.63 | 192x |
| `s2_cox` | 3.04x | 30.6k | 0.0336 | 4.15 | 124x |
| `ch09_m9_1_chains4` | 3.03x | 11.3k | 0.0159 | 3.32 | 209x |
| `logmesquite_logvolume` | 3.02x | 12.4k | 0.00914 | 3.05 | 334x |
| `s2_hurdle_cumulative` | 3x | 11.2k | 0.0418 | 4.21 | 101x |
| `radon_county` | 2.97x | 48.8k | 1.29 | 6.91 | 5.37x |
| `ch15_m15_1` | 2.94x | 30.7k | 0.02 | 3.43 | 171x |
| `eight_schools_centered` | 2.89x | 32.4k | 0.00986 | 3.01 | 305x |
| `s2_gr_student` | 2.87x | 52.2k | 0.0459 | 5.36 | 117x |
| `dogs_nonhierarchical` | 2.83x | 56.4k | 0.889 | 9.27 | 10.4x |
| `aalto_grp_prior_mean` | 2.81x | 19.3k | 0.00989 | 3.15 | 318x |
| `ch15_m15_5` | 2.79x | 21.5k | 0.0165 | 3.7 | 224x |
| `sw_acat` | 2.73x | 12.2k | 0.602 | 5.55 | 9.22x |
| `cm_geg` | 2.72x | 54k | 1.37 | 8.23 | 6x |
| `sw_hurdle_lognormal` | 2.72x | 12.5k | 0.026 | 3.6 | 138x |
| `sw_acat_cs` | 2.71x | 18.3k | 1.29 | 8.51 | 6.6x |
| `ch09_m5_8s` | 2.71x | 1.37M | 0.616 | 4.7 | 7.64x |
| `kilpisjarvi` | 2.69x | 1.12M | 0.227 | 3.39 | 14.9x |
| `Mtbh_model` | 2.69x | 34.3k | 0.571 | 6.37 | 11.2x |
| `aalto_lin` | 2.66x | 18.2k | 0.0102 | 3.05 | 299x |
| `ch15_m15_2` | 2.63x | 32.4k | 0.0267 | 3.68 | 138x |
| `aalto_lin_std` | 2.63x | 11k | 0.00883 | 3.26 | 369x |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.63x | 17.3k | 0.00896 | 3.2 | 357x |
| `ch15_m15_9` | 2.62x | 11.3k | 0.0357 | 3.2 | 89.8x |
| `aalto_grp_prior_mean_var` | 2.61x | 59.3k | 0.0234 | 3.86 | 165x |
| `sw_hurdle_pois` | 2.59x | 10.4k | 0.0285 | 3.47 | 122x |
| `s2_mmc` | 2.57x | 35.4k | 0.0636 | 7.08 | 111x |
| `ch15_m15_8` | 2.57x | 10.7k | 0.0333 | 2.92 | 87.8x |
| `ch12_m12_5` | 2.56x | 37.4k | 38.1 | 101 | 2.65x |
| `s2_s_cc` | 2.56x | 30.5k | 0.0217 | 4.24 | 196x |
| `ch09_m5_8s2` | 2.55x | 1.11M | 0.529 | 4.37 | 8.25x |
| `hierarchical_gp` | 2.54x | 352k | 6.06 | 24.4 | 4.02x |
| `s2_shifted_lognormal` | 2.54x | 15.1k | 0.0163 | 3.92 | 240x |
| `pilots` | 2.53x | 551k | 0.192 | 3.99 | 20.8x |
| `s2_gp_approx` | 2.53x | 34k | 0.0332 | 5.53 | 166x |
| `dogs_hierarchical` | 2.5x | 9.37k | 0.174 | 3.15 | 18.1x |
| `s2_nlf` | 2.49x | 9.42k | 0.0165 | 4.38 | 266x |
| `ch12_m12_7` | 2.49x | 48.6k | 49.7 | 127 | 2.55x |
| `ldaK5` † | 2.47x | 5k | 12.8 | 34.1 | 2.67x |
| `nes` | 2.44x | 90.8k | 1.68 | 7.69 | 4.57x |
| `ch14_m14_3` | 2.4x | 65.2k | 0.594 | 7.27 | 12.2x |
| `ch15_m15_6` | 2.39x | 15.6k | 0.0114 | 3.16 | 279x |
| `s2_me2` | 2.38x | 71.6k | 0.132 | 6.94 | 52.6x |
| `sw_spline_s` | 2.38x | 37.6k | 0.0278 | 4.38 | 157x |
| `sw_re_bern` | 2.38x | 27.4k | 0.0288 | 4.69 | 163x |
| `kidscore_mom_work` | 2.35x | 40.5k | 0.113 | 3.34 | 29.6x |
| `losscurve_sislob` | 2.35x | 21.3k | 0.0327 | 4.74 | 145x |
| `i320_mi_nhanes` | 2.34x | 312k | 0.274 | 5.04 | 18.4x |
| `accel_gp` | 2.34x | 1.5M | 5.32 | 19 | 3.57x |
| `sw_re_slope` | 2.34x | 53.4k | 0.0686 | 6.94 | 101x |
| `normal_mixture` | 2.34x | 7.96k | 0.306 | 3.51 | 11.5x |
| `cm_lnr` | 2.33x | 41.9k | 4.82 | 24.6 | 5.1x |
| `kidscore_interaction` | 2.31x | 178k | 0.478 | 4.26 | 8.91x |
| `s2_mv_shared_re` | 2.3x | 36.8k | 0.0672 | 7.28 | 108x |
| `ch09_m9_3` | 2.3x | 10.8k | 0.00711 | 2.53 | 355x |
| `kidscore_interaction_c2` | 2.3x | 17.7k | 0.0545 | 3.27 | 60x |
| `ch11_m11_5` | 2.29x | 29.3k | 0.202 | 3.53 | 17.5x |
| `ch09_m9_5` | 2.29x | 398k | 0.0601 | 2.68 | 44.5x |
| `ch09_m9_4` | 2.28x | 1.55M | 0.214 | 3.03 | 14.2x |
| `i319_pois_re2` | 2.27x | 173k | 0.823 | 6.67 | 8.1x |
| `kidscore_interaction_z` | 2.27x | 24k | 0.0707 | 3.38 | 47.8x |
| `s2_cumulative_cloglog` | 2.27x | 20.3k | 0.0341 | 3.84 | 113x |
| `extra_hurdle_poisson` | 2.26x | 8.4k | 0.00849 | 2.6 | 307x |
| `kidscore_interaction_c` | 2.26x | 15.2k | 0.047 | 3.28 | 69.7x |
| `s2_gev` | 2.25x | 12.2k | 0.0358 | 3.88 | 108x |
| `sw_skewnormal` | 2.24x | 15.5k | 0.0252 | 3.94 | 156x |
| `ch09_m9_2` | 2.24x | 46.7k | 0.00953 | 2.55 | 268x |
| `s2_sar` | 2.24x | 10.9k | 0.0496 | 4.3 | 86.6x |
| `logearn_height_male` | 2.23x | 189k | 1.06 | 5.4 | 5.09x |
| `sw_vonmises` | 2.23x | 730k | 0.516 | 4.79 | 9.27x |
| `logearn_logheight_male` | 2.23x | 715k | 4.04 | 12 | 2.98x |
| `s2_mi_lognormal` | 2.22x | 14.8k | 0.021 | 4.56 | 217x |
| `sw_nonlinear` | 2.21x | 10.9k | 0.0138 | 3.9 | 283x |
| `Mh_model` | 2.2x | 53.8k | 0.912 | 5.29 | 5.8x |
| `ch14_m14_5` | 2.2x | 12k | 0.0346 | 3.07 | 88.8x |
| `sw_lognormal` | 2.2x | 8.28k | 0.0115 | 3.64 | 318x |
| `cm_rdm` | 2.2x | 67.7k | 8.37 | 28.7 | 3.43x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.19x | 44.4k | 2.12 | 7.76 | 3.66x |
| `kidscore_momhsiq` | 2.18x | 90.5k | 0.188 | 3.46 | 18.4x |
| `logearn_interaction` | 2.18x | 322k | 2.55 | 8.72 | 3.42x |
| `dugongs_model` | 2.18x | 33.4k | 0.023 | 3.08 | 134x |
| `s2_sar_error` | 2.17x | 11.4k | 0.0517 | 4.25 | 82.1x |
| `low_dim_gauss_mix` | 2.16x | 14k | 0.691 | 4.66 | 6.75x |
| `GLMM_Poisson_model` | 2.15x | 75.6k | 0.0533 | 3.93 | 73.8x |
| `logearn_interaction_z` | 2.14x | 22k | 0.182 | 3.67 | 20.2x |
| `ch13_m13_4b` | 2.14x | 34.4k | 0.246 | 3.81 | 15.5x |
| `ch13_m13_7` | 2.14x | 19.9k | 0.00725 | 2.32 | 320x |
| `ch13_m13_6` | 2.13x | 36.1k | 0.265 | 3.94 | 14.9x |
| `ch13_m13_4` | 2.12x | 34.4k | 0.25 | 3.92 | 15.7x |
| `ch13_m13_4nc` | 2.12x | 75.5k | 0.667 | 4.71 | 7.06x |
| `ch09_mp` | 2.11x | 38.9k | 0.00741 | 2.31 | 312x |
| `aalto_lin_std_t` | 2.1x | 13k | 0.0114 | 3.45 | 302x |
| `accel_splines` | 2.1x | 1.86M | 7.05 | 19.3 | 2.74x |
| `sw_re_pois` | 2.09x | 30k | 0.0344 | 4.57 | 133x |
| `sesame_one_pred_a` | 2.09x | 15.5k | 0.0188 | 2.87 | 153x |
| `s2_s_by` | 2.08x | 70.7k | 0.12 | 5.91 | 49.2x |
| `seeds_centered_model` | 2.06x | 41.5k | 0.036 | 3.96 | 110x |
| `sw_se` | 2.05x | 7.2k | 0.0114 | 3.46 | 305x |
| `ch11_m11_10` | 2.05x | 10.7k | 0.00966 | 3.24 | 336x |
| `sw_student` | 2.05x | 12.7k | 0.0161 | 3.79 | 236x |
| `ch14_m14_4` | 2.03x | 9.05k | 0.0218 | 2.89 | 133x |
| `logearn_height` | 2.02x | 138k | 0.531 | 3.95 | 7.44x |
| `radon_pooled` | 2.02x | 9.33k | 0.397 | 3.65 | 9.21x |
| `ch14_m14_4x` | 2.01x | 9.05k | 0.0218 | 2.89 | 133x |
| `kidscore_momiq` | 2.01x | 60.1k | 0.0905 | 3.1 | 34.2x |
| `log10earn_height` | 2.01x | 143k | 0.542 | 3.96 | 7.31x |
| `ldaK2` | 2.01x | 19.8k | 1.01 | 5.67 | 5.59x |
| `ch13_m13_7nc` | 2x | 7.88k | 0.00623 | 2.36 | 379x |
| `cm_lognormal` | 2x | 68k | 2.69 | 10.7 | 3.99x |
| `s2_nl_noloop` | 2x | 52.6k | 0.0332 | 3.85 | 116x |
| `earn_height` | 2x | 150k | 0.575 | 3.99 | 6.94x |
| `ch11_m11_4` | 1.99x | 16.5k | 0.109 | 3.25 | 29.9x |
| `sw_dist_sigma` | 1.99x | 10.7k | 0.017 | 3.78 | 223x |
| `kidscore_momhs` | 1.99x | 24.8k | 0.0421 | 3 | 71.4x |
| `sw_zi_poisson` | 1.98x | 11.5k | 0.0254 | 3.44 | 135x |
| `GLM_Poisson_model` | 1.98x | 29.5k | 0.0192 | 3.31 | 172x |
| `normal_mixture_k` | 1.97x | 243k | 48.1 | 98.3 | 2.04x |
| `ch11_m11_6` | 1.96x | 16.7k | 0.0152 | 3.03 | 199x |
| `sw_spline_t2` | 1.95x | 38.1k | 0.0468 | 4.87 | 104x |
| `ch11_m11_8` | 1.95x | 50.9k | 0.0219 | 3.01 | 137x |
| `s2_index_mi` | 1.94x | 18.6k | 0.0264 | 4.22 | 159x |
| `ch16_m16_4` | 1.93x | 35.3k | 0.0636 | 3.46 | 54.4x |
| `ch13_m13_5` | 1.93x | 18.9k | 0.125 | 3.44 | 27.6x |
| `cm_bisa` | 1.92x | 27.9k | 0.646 | 5.34 | 8.27x |
| `election88_full` | 1.91x | 429k | 104 | 204 | 1.96x |
| `ch13_m13_1` | 1.91x | 18.5k | 0.0201 | 3.01 | 150x |
| `i319_pois_re` | 1.9x | 192k | 0.705 | 5.82 | 8.25x |
| `ch13_m13_2` | 1.9x | 22.9k | 0.024 | 3.18 | 132x |
| `ch13_m13_3` | 1.89x | 27.8k | 0.0302 | 3.06 | 101x |
| `cm_loggamma` | 1.88x | 226k | 6.78 | 17.3 | 2.55x |
| `multi_occupancy` | 1.88x | 98.9k | 3.03 | 12.1 | 3.99x |
| `s2_t2_by` | 1.87x | 36.4k | 0.0735 | 5.77 | 78.5x |
| `cm_lnr_bench` | 1.83x | 40.1k | 10.7 | 27.7 | 2.58x |
| `s2_cens_interval` | 1.8x | 8.94k | 0.0243 | 3.76 | 154x |
| `ch11_m11_11` | 1.8x | 143k | 0.0605 | 3.64 | 60.2x |
| `sw_mi` | 1.8x | 15.1k | 0.0198 | 3.84 | 194x |
| `lotka_volterra` | 1.8x | 75.1k | 2.58 | 9.11 | 3.54x |
| `sw_hurdle_gamma` | 1.8x | 13.3k | 0.0383 | 3.57 | 93.1x |
| `cm_logweibull` | 1.8x | 90.4k | 1.87 | 7.37 | 3.94x |
| `soil_incubation` | 1.8x | 203k | 4.97 | 12.8 | 2.58x |
| `cm_lba1` | 1.75x | 145k | 8 | 18.8 | 2.36x |
| `sw_asymlaplace` | 1.75x | 23.4k | 0.0581 | 3.7 | 63.7x |
| `ch15_m15_3` | 1.74x | 7.74k | 0.109 | 2.81 | 25.9x |
| `bym2_offset_only` | 1.73x | 202k | 7.66 | 17.6 | 2.3x |
| `cm_weibull` | 1.7x | 2.03M | 37.5 | 67.7 | 1.81x |
| `cm_betagate` | 1.69x | 11.8k | 0.419 | 4.89 | 11.7x |
| `sw_zi_binomial` | 1.69x | 10.2k | 0.0344 | 3.46 | 101x |
| `hier_2pl` | 1.69x | 59.4k | 11.9 | 26.8 | 2.25x |
| `seeds_stanified_model` | 1.69x | 42.2k | 0.0354 | 3.69 | 104x |
| `ch15_m15_4` | 1.68x | 8.81k | 0.101 | 2.8 | 27.7x |
| `s2_threading` | 1.66x | 11.8k | 0.0119 | 3.37 | 283x |
| `seeds_model` | 1.66x | 38.4k | 0.0331 | 3.77 | 114x |
| `sw_ar` | 1.65x | 10.4k | 0.0273 | 4.26 | 156x |
| `sw_ma` | 1.64x | 11.1k | 0.0288 | 4.26 | 148x |
| `ch11_m11_7` | 1.63x | 7.03k | 0.00827 | 2.85 | 344x |
| `sw_arma` | 1.63x | 42.2k | 0.105 | 4.48 | 42.6x |
| `cm_logstudent` | 1.63x | 2.02M | 30.6 | 53.9 | 1.76x |
| `2pl_latent_reg_irt` | 1.62x | 51.2k | 3.4 | 11.5 | 3.37x |
| `s2_mv_subset` | 1.61x | 12.1k | 0.0164 | 3.5 | 214x |
| `cm_gamma` | 1.6x | 2.01M | 41.9 | 71.2 | 1.7x |
| `cm_lba2` | 1.6x | 140k | 13.6 | 29.2 | 2.14x |
| `cm_invweibull` | 1.6x | 87.1k | 2.02 | 7.24 | 3.59x |
| `surgical_model` | 1.59x | 22.2k | 0.0162 | 3.43 | 212x |
| `sir` | 1.57x | 127k | 20.8 | 36.7 | 1.76x |
| `cm_invgamma` | 1.56x | 79k | 1.82 | 6.88 | 3.77x |
| `cm_choco` | 1.54x | 15.1k | 0.69 | 6.04 | 8.75x |
| `lsat_model` | 1.53x | 40.8k | 1.84 | 6.53 | 3.56x |
| `s2_unstr` | 1.52x | 46.4k | 0.224 | 8.98 | 40.1x |
| `sw_gaussian` | 1.52x | 10.9k | 0.0111 | 3.31 | 299x |
| `Rate_4_model` | 1.51x | 8.3k | 0.00752 | 2.52 | 335x |
| `s2_categorical_re` | 1.5x | 34.6k | 0.182 | 5.63 | 31x |
| `sw_re_negbin` | 1.5x | 30.3k | 0.0661 | 4.85 | 73.4x |
| `sw_cratio_cs` | 1.5x | 13.7k | 1.25 | 6.83 | 5.47x |
| `sw_mv_norescor` | 1.5x | 11.6k | 0.0164 | 3.51 | 214x |
| `Mb_model` | 1.49x | 11k | 0.434 | 3.88 | 8.94x |
| `sw_gamma` | 1.49x | 9.45k | 0.0149 | 3.77 | 253x |
| `i320_sratio_cs` | 1.48x | 20.4k | 1.83 | 8 | 4.38x |
| `gp_regr` | 1.46x | 8.82k | 0.0277 | 5.46 | 197x |
| `s2_rate` | 1.46x | 7.8k | 0.0128 | 3.58 | 279x |
| `s2_mixture_theta` | 1.46x | 80.4k | 0.454 | 5.06 | 11.2x |
| `sw_zi_negbin` | 1.45x | 20.5k | 0.073 | 3.57 | 48.9x |
| `covid19imperial_v3` | 1.45x | 473k | 118 | 177 | 1.5x |
| `covid19imperial_v2` | 1.45x | 473k | 118 | 177 | 1.5x |
| `gp_pois_regr` | 1.45x | 396k | 0.743 | 6.7 | 9.01x |
| `s2_gp_by_gr` | 1.44x | 59.6k | 0.431 | 10.6 | 24.6x |
| `prophet` | 1.44x | 1.66M | 61.9 | 94.8 | 1.53x |
| `s2_zoi_beta` | 1.44x | 12.1k | 0.0612 | 3.85 | 62.8x |
| `ch11_m11_9` | 1.44x | 5.08k | 0.00718 | 2.6 | 362x |
| `s2_zi_asymlaplace` | 1.42x | 28.3k | 0.0969 | 3.81 | 39.3x |
| `Rate_1_model` | 1.42x | 4.69k | 0.00804 | 2.39 | 297x |
| `aalto_binom` | 1.42x | 5.11k | 0.00635 | 2.41 | 380x |
| `aalto_binom2` | 1.42x | 7.7k | 0.00776 | 2.5 | 322x |
| `ch14_m14_2` | 1.42x | 135k | 2.26 | 10.3 | 4.54x |
| `hmm_example` | 1.42x | 25.1k | 0.509 | 4.79 | 9.41x |
| `Rate_2_model` | 1.41x | 7.31k | 0.00816 | 2.58 | 317x |
| `ch15_m15_7` | 1.41x | 28k | 0.26 | 7.91 | 30.4x |
| `aalto_bern` | 1.4x | 5.11k | 0.00651 | 2.55 | 391x |
| `s2_zi_beta` | 1.4x | 10.2k | 0.0519 | 3.77 | 72.6x |
| `sw_exgaussian` | 1.4x | 25.1k | 0.0315 | 3.89 | 124x |
| `s2_cumulative_probit` | 1.39x | 13.7k | 0.0668 | 3.76 | 56.3x |
| `sw_cumulative_cs` | 1.39x | 22.3k | 1.45 | 6.96 | 4.79x |
| `ch11_m_pois` | 1.38x | 6.85k | 0.0072 | 2.53 | 351x |
| `Rate_5_model` | 1.38x | 5.33k | 0.00669 | 2.49 | 372x |
| `ch12_m12_6` | 1.37x | 111k | 219 | 303 | 1.39x |
| `blr` | 1.36x | 32.4k | 0.0185 | 3.06 | 165x |
| `s2_com_poisson` | 1.36x | 42.5k | 0.886 | 5.81 | 6.55x |
| `i320_gp_expquad` | 1.36x | 52.4k | 0.571 | 8.04 | 14.1x |
| `Rate_3_model` | 1.36x | 5.2k | 0.00755 | 2.4 | 318x |
| `aalto_binomb` | 1.35x | 5.61k | 0.00687 | 2.42 | 352x |
| `ch14_m14_8nc` | 1.35x | 263k | 0.731 | 6.95 | 9.51x |
| `i319_negbin_re` | 1.35x | 66.7k | 0.656 | 5.62 | 8.57x |
| `ch12_m12_2` | 1.33x | 64.2k | 0.0635 | 4.07 | 64.2x |
| `wells_dist` | 1.33x | 26.5k | 0.664 | 3.87 | 5.82x |
| `GLM_Binomial_model` | 1.32x | 13.3k | 0.0189 | 3.31 | 175x |
| `ch12_m12_1` | 1.31x | 11.8k | 0.0198 | 3.25 | 164x |
| `sw_mixture` | 1.29x | 303k | 1.6 | 6.78 | 4.25x |
| `sw_beta` | 1.25x | 8.99k | 0.0247 | 3.92 | 159x |
| `ch14_m14_1` | 1.25x | 37.6k | 0.206 | 7.12 | 34.6x |
| `sw_gp` | 1.25x | 47.3k | 0.921 | 8.39 | 9.11x |
| `nn_rbm1bJ10` | 1.25x | 2.03M | 326 | 412 | 1.26x |
| `ch14_m14_9` | 1.24x | 12.5k | 4.33 | 9.57 | 2.21x |
| `ch14_m14_6x` | 1.24x | 21.6k | 1.91 | 9.07 | 4.75x |
| `ch14_m14_6` | 1.24x | 21.6k | 1.9 | 9.06 | 4.77x |
| `ch14_m14_8` | 1.22x | 204k | 0.576 | 5.45 | 9.45x |
| `sw_mv_rescor` | 1.22x | 13.3k | 0.0901 | 6.19 | 68.7x |
| `ch14_m14_10` | 1.21x | 11.2k | 4.21 | 9.3 | 2.21x |
| `sw_categorical` | 1.18x | 11k | 0.022 | 4.37 | 198x |
| `s2_frechet` | 1.18x | 33.2k | 0.0737 | 4.03 | 54.6x |
| `s2_custom_vint` | 1.18x | 10.5k | 0.0608 | 3.58 | 58.8x |
| `Survey_model` | 1.16x | 11.1k | 0.65 | 3.7 | 5.7x |
| `ch14_m14_7` | 1.16x | 143k | 4.03 | 13.5 | 3.34x |
| `sw_bernoulli` | 1.16x | 8.63k | 0.0117 | 3.27 | 279x |
| `sw_weibull` | 1.15x | 10.3k | 0.0291 | 3.91 | 134x |
| `sw_binomial` | 1.14x | 7.52k | 0.0225 | 3.52 | 156x |
| `sw_poisson` | 1.13x | 9.85k | 0.014 | 3.14 | 225x |
| `s2_multinomial` | 1.13x | 12.9k | 0.129 | 4.1 | 31.9x |
| `i320_sratio_plain` | 1.13x | 15.4k | 0.929 | 5.1 | 5.49x |
| `sw_sratio` | 1.11x | 10.4k | 0.626 | 4.58 | 7.31x |
| `sw_cratio` | 1.1x | 10.4k | 0.631 | 4.57 | 7.25x |
| `s2_beta_binomial` | 1.09x | 10.4k | 0.0532 | 3.75 | 70.4x |
| `hmm_gaussian` | 1.08x | 63.9k | 17.2 | 23.3 | 1.35x |
| `nn_rbm1bJ100` † | 1.07x | 5k | 2.2e+03 | 2.36e+03 | 1.07x |
| `sw_mono` | 1.07x | 20.4k | 0.0438 | 4.13 | 94.3x |
| `ch14_m14_11` | 1.07x | 12.7k | 7.87 | 12.8 | 1.62x |
| `s2_discrete_weibull` | 1.07x | 31.8k | 0.12 | 3.59 | 30x |
| `s2_weights_trunc` | 1.06x | 25.1k | 0.172 | 3.74 | 21.7x |
| `sw_negbinomial` | 1.06x | 1.49M | 2.07 | 5.59 | 2.69x |
| `sw_trunc` | 1.06x | 27.2k | 0.177 | 3.71 | 21x |
| `bones_model` | 1.04x | 14.7k | 0.776 | 4.31 | 5.55x |
| `s2_dirichlet` | 1.04x | 12.6k | 0.17 | 4.52 | 26.5x |
| `s2_hurdle_negbin` | 1.04x | 15.2k | 0.111 | 3.59 | 32.4x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0738 | 3.19 | 43.3x |
| `hmm_drive_1` | 1.04x | 39.6k | 5.95 | 10.9 | 1.83x |
| `wells_dist100_model` | 1.03x | 11.5k | 0.206 | 3.41 | 16.6x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.325 | 3.71 | 11.4x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.02x | 11.2k | 0.235 | 3.53 | 15x |
| `i320_pois_trunc_ub` | 1.01x | 21.9k | 0.58 | 4.03 | 6.94x |
| `nes_logit_model` | 1.01x | 17.8k | 0.127 | 3.29 | 25.9x |
| `wells_dist100ars_model` | 1.01x | 19.1k | 0.367 | 3.59 | 9.78x |
| `s2_mo_simo_prior` | 1.01x | 21.3k | 0.0473 | 4.16 | 87.8x |
| `wells_daae_c_model` | 1.01x | 17.6k | 0.375 | 3.74 | 9.97x |
| `aalto_poisson_simple` | 1.01x | 5.02k | 0.0128 | 2.66 | 208x |
| `wells_dae_model` | 1.01x | 24.2k | 0.502 | 3.75 | 7.47x |
| `wells_interaction_model` | 1x | 32.1k | 0.668 | 3.89 | 5.82x |
| `i319_negbin_fixed` | 1x | 19.4k | 0.187 | 3.56 | 19x |
| `s2_wiener` | 1x | 14.7k | 0.37 | 4.04 | 10.9x |
| `diamonds` | 0.999x | 1.51M | 49.6 | 53 | 1.07x |
| `cm_betadiscrete` | 0.996x | 9.91k | 22.9 | 27.1 | 1.18x |
| `sw_cumulative` | 0.996x | 9.89k | 0.226 | 3.97 | 17.6x |
| `wells_dae_inter_model` | 0.988x | 12.8k | 0.284 | 3.62 | 12.8x |
| `one_comp_mm_elim_abs` | 0.981x | 18.9k | 8.84 | 12.5 | 1.41x |
| `i320_pois_trunc_both` | 0.978x | 21.9k | 0.731 | 4.21 | 5.77x |
| `iohmm_reg` | 0.978x | 462k | 156 | 159 | 1.02x |
| `cm_ddm` † | 0.974x | 5k | 855 | 842 | 0.985x |
| `s2_ar_cov` | 0.97x | 11.2k | 0.0896 | 7.61 | 84.9x |
| `s2_cosy` | 0.961x | 14.7k | 0.102 | 6.81 | 66.6x |
| `s2_fcor` | 0.95x | 9.87k | 0.148 | 4.99 | 33.8x |
| `hmm_drive_0` | 0.945x | 24.5k | 3.7 | 8.26 | 2.23x |
| `i320_gp_matern32` | 0.917x | 133k | 4.23 | 11 | 2.6x |
| `cm_invgaussian` † | 0.845x | 5k | 0.872 | 33.8 | 38.7x |
| `garch11` | 0.807x | 23.3k | 0.201 | 3.1 | 15.4x |
| `aalto_gpareto` | 0.751x | 9.37k | 0.0157 | 3.09 | 197x |
| `arma11` | 0.727x | 11.2k | 0.0687 | 3 | 43.7x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling | Reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `irt_2pl` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 4.71e-08 |
| `kronecker_gp` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.0723 |
| `logistic_regression_rhs` | — | — | — | — | — | failed; point 2: density/gradient mismatch: scaled error 3.79e-08 |
| `s2_mi_trunc_lb` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 6.93e-05 |
| `sw_cens` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 1.02e-06 |
<!--/gen-->

## Fast mode

<!--gen:fast_lead-->
The fast mode run is from 2026-10-10 (Stanli source `84e60796`), with models
compiled and run in fast mode (`corpus_bench.py --fast-math`). The default
mode run is from 2026-10-20 (Stanli source `b61aca02`). The two runs used
different binaries, so the comparison below is between runs.
<!--/gen-->
Gradient times, <!--gen:fast_population-->334<!--/gen--> models with paired timings, Apple M3 Ultra:

- Fast mode is <!--gen:fast_geomean-->1.87x<!--/gen--> faster than default mode in geometric mean; the median
  model is <!--gen:fast_median-->1.03x<!--/gen--> faster. <!--gen:fast_ge_two-->91<!--/gen--> models are at least 2x faster, <!--gen:fast_ge_ten-->42<!--/gen--> at least 10x
  and <!--gen:fast_ge_hundred-->7<!--/gen--> at least 100x. Most of the large gains are models whose likelihood
  repeats over the data, which fast mode evaluates once per distinct row or
  group.
- Against CmdStan, the geometric-mean speedup is <!--gen:fast_cmdstan_default-->2.25x<!--/gen--> in default mode and
  <!--gen:fast_cmdstan_fast-->3.96x<!--/gen--> in fast mode. Fast mode beats CmdStan on <!--gen:fast_beats_fast-->319<!--/gen--> of the <!--gen:fast_population-->334<!--/gen--> models,
  default mode on <!--gen:fast_beats_default-->321<!--/gen-->.
- <!--gen:fast_slowest-->The slowest model in fast mode, `s2_com_poisson`, is 11.53x slower.<!--/gen-->

The largest gains, gradient time in microseconds:

<!--gen:fast_top_table-->
| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 41.3 | 0.0812 | 508x faster |
| `nes` | 18.4 | 0.0723 | 255x faster |
| `logearn_interaction` | 7.9 | 0.05 | 158x faster |
| `logearn_interaction_z` | 7.93 | 0.0511 | 155x faster |
| `logearn_logheight_male` | 5.64 | 0.0438 | 129x faster |
| `logearn_height_male` | 5.56 | 0.0438 | 127x faster |
| `diamonds` | 32.9 | 0.311 | 106x faster |
| `ch12_m12_4` | 66.6 | 0.666 | 100x faster |
<!--/gen-->

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

Five models fail the gate in both tables and appear in the incomplete
results. `kronecker_gp`, `s2_mi_trunc_lb` and `sw_cens` have scaled errors of
`0.072`, `6.9e-5` and `1.0e-6`; `irt_2pl` and `logistic_regression_rhs` have
`4.7e-8` and `3.8e-8`. No model drops a point.
The `kronecker_gp` gradient passes through `eigenvectors_sym`, which is
ill-conditioned for its first covariance matrix. At the ten benchmark points
the smallest eigenvalue gap is 6e-18 to 5e-17, which is 1e-18 to 8e-18 of the
eigenvalue and below double precision
([known limits](../TESTING.md#known-limits)).
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
| Measured source | <!--gen:benchmark_source-->`b61aca0299627f7e1d79c5b949d6b923c56163ef`<!--/gen-->; tracked differences and executable hashes in manifests |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan | <!--gen:benchmark_cmdstan-->`11cb052d3e1fc8c799e0fec559e2ee5452b38d27` (not the pinned 2.40.0)<!--/gen--> |
| Stan Math | <!--gen:benchmark_stan_math-->`8f326d14599d3030c626c46532d8e8534c1cdbec`<!--/gen--> |
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
The harness refuses a CmdStan checkout that differs from `deps/pins.env` in its CmdStan,
Stan or Stan Math head, its tracked files or its `bin/stanc`; `--allow-unpinned-cmdstan`
overrides that and the manifest records it.
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

---

<sub>On macOS, CmdStan's makefile links TBB's malloc replacement
(`tbbmalloc_proxy`) into every model. The benchmark's CmdStan driver links
only `tbb`, as CmdStan does on Linux, and that makes CmdStan faster than a
default Mac build on allocation-heavy models. In a spot check the default build
was 1.7x slower on `ch12_m12_4` and 1.5x slower on `gpcm_latent_reg_irt`.</sub>
