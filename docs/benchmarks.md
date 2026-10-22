# Benchmarks

Stanli has **<!--gen:corpus_median-->2.15x<!--/gen-->** faster gradients and **<!--gen:corpus_total_median-->23x<!--/gen-->** faster total sampling than CmdStan
for the median of <!--gen:benchmark_models-->343<!--/gen--> application models.

Stanli runs Stan models without a per-model C++ build. Total sampling is
setup plus the gradients of a full 1,000 warmup + 1,000 draw run, so it
includes CmdStan's C++ build for each model. This page compares the two
engines model by model.

## TL;DR

- The median model's gradient is **<!--gen:corpus_median-->2.15x<!--/gen--> faster** in Stanli than in
  CmdStan. Stanli is as fast or faster on <!--gen:corpus_at_par-->327<!--/gen--> 
  of the <!--gen:corpus_n_grad-->342<!--/gen--> models that pass the numerical check.
- In the total-sampling column, which adds setup to the gradients a full
  1,000 warmup + 1,000 draw run takes, the median model is <!--gen:corpus_total_median-->23x<!--/gen--> faster, mostly because
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
Run `144302fd2f39cf47` (2026-10-21): 343 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 6 of 343 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 32x | 18.6k | 1.29 | 43.2 | 33.5x |
| `gpcm_latent_reg_irt` | 12.5x | 69k | 7.97 | 103 | 12.9x |
| `grsm_latent_reg_irt` | 12x | 63.1k | 4.23 | 53.9 | 12.8x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 47.3k | 1.26 | 15.5 | 12.4x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11x | 68.5k | 1.87 | 21.9 | 11.7x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.2x | 50.6k | 2.3 | 25 | 10.8x |
| `radon_county_intercept` | 10.1x | 62.8k | 2.85 | 30 | 10.5x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 10x | 52.9k | 2.47 | 25.9 | 10.5x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.85x | 102k | 4.85 | 49 | 10.1x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.78x | 81.2k | 3.78 | 38.7 | 10.2x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.4x | 77.7k | 5.12 | 48.9 | 9.55x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.29x | 100k | 6.58 | 62.7 | 9.53x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.16x | 59.7k | 3.11 | 29.8 | 9.6x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.15x | 125k | 6.5 | 61.2 | 9.43x |
| `radon_pooled` | 8.31x | 9.33k | 0.401 | 4.7 | 11.7x |
| `aalto_poisson_hurdle` | 8.08x | 6.92k | 0.0783 | 2.1 | 26.8x |
| `s2_custom_vreal` | 7.86x | 10.2k | 0.0122 | 2.28 | 187x |
| `dogs` | 7.66x | 20.7k | 0.205 | 3.76 | 18.3x |
| `rats_model` | 7.4x | 54.5k | 0.0484 | 2.12 | 43.8x |
| `arK` | 7.31x | 59.1k | 0.11 | 2.36 | 21.6x |
| `ch12_m12_3` | 6.88x | 10.8k | 0.0395 | 1.45 | 36.7x |
| `ch16_m16_1` | 6.8x | 301k | 1.23 | 10 | 8.16x |
| `M0_model` | 6.66x | 7.46k | 0.028 | 1.56 | 55.7x |
| `ch12_m12_3_alt` | 6.63x | 11k | 0.0387 | 1.46 | 37.8x |
| `Mt_model` | 5.77x | 11k | 0.0503 | 2.42 | 48.2x |
| `sw_nonlinear` | 5.31x | 10.9k | 0.0141 | 2.69 | 190x |
| `dogs_log` | 5.13x | 12.1k | 0.128 | 2.51 | 19.7x |
| `ctsem_ctsm_400` † | 5.07x | 5k | 20.4 | 187 | 9.2x |
| `ctsem_ctsm` | 5.02x | 1.08M | 438 | 2.27e+03 | 5.19x |
| `ch09_m5_8s` | 4.82x | 1.37M | 0.629 | 4.59 | 7.3x |
| `ch09_m5_8s2` | 4.69x | 1.11M | 0.53 | 4.02 | 7.58x |
| `s2_invgaussian` | 4.63x | 16.3k | 0.0147 | 2.53 | 172x |
| `ch14_m14_5` | 4.6x | 12k | 0.0357 | 1.69 | 47.4x |
| `s2_cumulative_cauchit` | 4.4x | 20.7k | 0.0311 | 2.51 | 80.8x |
| `mesquite` | 4.35x | 160k | 0.0794 | 2.13 | 26.8x |
| `logmesquite_logvash` | 4.24x | 93.7k | 0.045 | 2.23 | 49.5x |
| `ch15_m15_1` | 4.23x | 30.7k | 0.0194 | 2 | 103x |
| `logmesquite_logvas` | 4.22x | 92.3k | 0.0502 | 2.3 | 45.9x |
| `s2_nl_noloop` | 4.16x | 52.6k | 0.0346 | 2.81 | 81.1x |
| `logmesquite` | 4.12x | 56.4k | 0.0343 | 2.01 | 58.5x |
| `ch15_m15_2` | 4.07x | 32.4k | 0.0267 | 2.24 | 83.7x |
| `s2_logistic_normal` | 4.06x | 13.6k | 0.0836 | 5.41 | 64.7x |
| `election88_full` | 3.98x | 429k | 103 | 413 | 4.02x |
| `logmesquite_logva` | 3.95x | 58.7k | 0.0244 | 1.98 | 81.1x |
| `s2_car_icar` | 3.94x | 1.7M | 0.744 | 6.1 | 8.2x |
| `nes` | 3.94x | 90.8k | 1.69 | 8.57 | 5.07x |
| `kidscore_interaction_c` | 3.92x | 15.2k | 0.0467 | 1.92 | 41.1x |
| `ch14_m14_4x` | 3.86x | 9.05k | 0.0222 | 1.6 | 71.8x |
| `kidscore_interaction` | 3.82x | 178k | 0.473 | 3.55 | 7.51x |
| `ch14_m14_4` | 3.82x | 9.05k | 0.0219 | 1.6 | 72.8x |
| `sw_me` | 3.82x | 57.9k | 0.0408 | 3.02 | 73.9x |
| `Mth_model` | 3.81x | 33.9k | 0.89 | 6.38 | 7.17x |
| `s2_nlf` | 3.75x | 9.42k | 0.0168 | 2.78 | 165x |
| `logmesquite_logvolume` | 3.75x | 12.4k | 0.0101 | 1.75 | 173x |
| `kidscore_mom_work` | 3.72x | 40.5k | 0.114 | 2.07 | 18.1x |
| `s2_me2_nomecor` | 3.71x | 50.4k | 0.0571 | 3.36 | 58.9x |
| `sw_spline_s` | 3.7x | 37.6k | 0.0283 | 3.3 | 116x |
| `sw_dist_sigma` | 3.66x | 10.7k | 0.0167 | 2.64 | 158x |
| `s2_s_cc` | 3.65x | 30.5k | 0.0226 | 3.13 | 139x |
| `ch09_m9_1_chains4` | 3.63x | 11.3k | 0.0148 | 1.86 | 125x |
| `s2_dist_sigma_re` | 3.62x | 29.4k | 0.0311 | 3.77 | 121x |
| `sw_weights` | 3.59x | 9.98k | 0.014 | 2.3 | 164x |
| `kidscore_interaction_c2` | 3.58x | 17.7k | 0.0542 | 1.91 | 35.2x |
| `ch09_m9_1` | 3.58x | 11.3k | 0.015 | 1.86 | 124x |
| `kidscore_interaction_z` | 3.56x | 24k | 0.0707 | 2.05 | 29x |
| `kilpisjarvi` | 3.49x | 1.12M | 0.233 | 2.33 | 10x |
| `s2_cox_cens` | 3.46x | 22.1k | 0.0314 | 3.92 | 125x |
| `logearn_logheight_male` | 3.44x | 715k | 4.01 | 15.4 | 3.85x |
| `aalto_grp_aov` | 3.4x | 12.5k | 0.00812 | 1.82 | 224x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.39x | 1.03M | 6.38 | 25.3 | 3.96x |
| `sw_se` | 3.38x | 7.2k | 0.0119 | 2.3 | 193x |
| `logearn_interaction_z` | 3.38x | 22k | 0.182 | 2.46 | 13.5x |
| `s2_gp_approx` | 3.38x | 34k | 0.0327 | 4.06 | 124x |
| `logearn_height_male` | 3.37x | 189k | 1.08 | 5.28 | 4.88x |
| `s2_shifted_lognormal` | 3.37x | 15.1k | 0.0161 | 2.74 | 170x |
| `s2_mm` | 3.37x | 26.6k | 0.0281 | 3.49 | 124x |
| `logearn_interaction` | 3.36x | 322k | 2.56 | 10.3 | 4.03x |
| `ch15_m15_8` | 3.36x | 10.7k | 0.0321 | 1.67 | 51.9x |
| `s2_mm_weights` | 3.34x | 27.8k | 0.0289 | 3.5 | 121x |
| `ch16_m16_4` | 3.33x | 35.3k | 0.0639 | 1.84 | 28.8x |
| `s2_car` | 3.32x | 30.6k | 0.032 | 3.23 | 101x |
| `s2_s_by` | 3.29x | 70.7k | 0.119 | 5 | 42x |
| `radon_county` | 3.27x | 48.8k | 1.28 | 6.02 | 4.7x |
| `GLMM1_model` | 3.27x | 32.8k | 0.333 | 3.05 | 9.16x |
| `sw_spline_t2` | 3.27x | 38.1k | 0.0461 | 3.8 | 82.5x |
| `s2_t2_by` | 3.26x | 36.4k | 0.0732 | 4.79 | 65.4x |
| `sw_lognormal` | 3.25x | 8.28k | 0.0127 | 2.5 | 196x |
| `ch15_m15_9` | 3.24x | 11.3k | 0.0349 | 1.82 | 52x |
| `s2_car_esicar` | 3.22x | 28.9k | 0.0289 | 3.33 | 115x |
| `ch15_m15_5` | 3.19x | 21.5k | 0.017 | 1.99 | 117x |
| `s2_gp_by_approx` | 3.17x | 37.2k | 0.0552 | 4.89 | 88.7x |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.17x | 17.3k | 0.00905 | 2.02 | 223x |
| `i319_gauss_re` | 3.17x | 41.3k | 0.0715 | 3.53 | 49.4x |
| `kidscore_momhsiq` | 3.15x | 90.5k | 0.189 | 2.25 | 11.9x |
| `s2_gr_by` | 3.15x | 33.2k | 0.0278 | 3.77 | 136x |
| `s2_cox` | 3.1x | 30.6k | 0.0347 | 3 | 86.6x |
| `cm_exwald` | 3.05x | 68k | 3.03 | 12 | 3.97x |
| `sw_re_gauss` | 3.01x | 29.1k | 0.0241 | 3.41 | 142x |
| `sw_student` | 2.9x | 12.7k | 0.0158 | 2.66 | 169x |
| `s2_mi_lognormal` | 2.9x | 14.8k | 0.0217 | 3.4 | 157x |
| `earn_height` | 2.88x | 150k | 0.562 | 3.14 | 5.59x |
| `aalto_grp_prior_mean` | 2.88x | 19.3k | 0.0102 | 1.9 | 187x |
| `bym2_offset_only` | 2.88x | 202k | 7.66 | 24.8 | 3.24x |
| `sw_acat` | 2.87x | 12.2k | 0.63 | 4.41 | 7x |
| `s2_hurdle_cumulative` | 2.87x | 11.2k | 0.0408 | 3.01 | 73.8x |
| `log10earn_height` | 2.86x | 143k | 0.532 | 3.08 | 5.79x |
| `logearn_height` | 2.86x | 138k | 0.526 | 3.07 | 5.84x |
| `kidscore_momiq` | 2.84x | 60.1k | 0.0908 | 1.85 | 20.3x |
| `ch15_m15_6` | 2.84x | 15.6k | 0.0115 | 1.72 | 149x |
| `sesame_one_pred_a` | 2.84x | 15.5k | 0.0197 | 1.57 | 80x |
| `Mtbh_model` | 2.83x | 34.3k | 0.566 | 5.3 | 9.37x |
| `s2_cumulative_cloglog` | 2.82x | 20.3k | 0.0342 | 2.5 | 73.3x |
| `s2_gr_student` | 2.8x | 52.2k | 0.0453 | 4.05 | 89.3x |
| `pilots` | 2.79x | 551k | 0.194 | 2.64 | 13.6x |
| `GLMM_Poisson_model` | 2.78x | 75.6k | 0.0529 | 2.7 | 51.1x |
| `sw_hurdle_lognormal` | 2.77x | 12.5k | 0.0257 | 2.4 | 93.2x |
| `dogs_nonhierarchical` | 2.76x | 56.4k | 0.89 | 8.14 | 9.14x |
| `blr` | 2.76x | 32.4k | 0.0186 | 1.92 | 103x |
| `kidscore_momhs` | 2.74x | 24.8k | 0.0418 | 1.69 | 40.5x |
| `GLM_Poisson_model` | 2.73x | 29.5k | 0.0188 | 2.08 | 111x |
| `s2_mmc` | 2.72x | 35.4k | 0.0629 | 5.93 | 94.2x |
| `ch13_m13_4nc` | 2.68x | 75.5k | 0.678 | 3.85 | 5.69x |
| `accel_gp` | 2.68x | 1.5M | 5.3 | 18.3 | 3.45x |
| `sw_gamma` | 2.64x | 9.45k | 0.0143 | 2.54 | 178x |
| `aalto_grp_prior_mean_var` | 2.64x | 59.3k | 0.0247 | 2.72 | 110x |
| `aalto_lin` | 2.63x | 18.2k | 0.0098 | 1.83 | 187x |
| `aalto_lin_std` | 2.62x | 11k | 0.00954 | 2.04 | 214x |
| `cm_geg` | 2.62x | 54k | 1.37 | 6.17 | 4.51x |
| `eight_schools_centered` | 2.61x | 32.4k | 0.0103 | 1.73 | 168x |
| `sw_hurdle_pois` | 2.6x | 10.4k | 0.0293 | 2.2 | 75x |
| `s2_gev` | 2.6x | 12.2k | 0.0357 | 2.62 | 73.4x |
| `hierarchical_gp` | 2.58x | 352k | 6.01 | 23.3 | 3.87x |
| `s2_me2` | 2.57x | 71.6k | 0.131 | 5.85 | 44.5x |
| `accel_splines` | 2.57x | 1.86M | 7.07 | 21.1 | 2.98x |
| `ch12_m12_7` | 2.56x | 48.6k | 50.3 | 131 | 2.6x |
| `ch12_m12_5` | 2.53x | 37.4k | 39.1 | 102 | 2.6x |
| `cm_exgaussian` | 2.52x | 14k | 0.222 | 2.94 | 13.2x |
| `s2_sar` | 2.51x | 10.9k | 0.0491 | 2.95 | 60.1x |
| `ldaK5` † | 2.5x | 5k | 12.6 | 32.8 | 2.6x |
| `losscurve_sislob` | 2.49x | 21.3k | 0.0331 | 3.1 | 93.9x |
| `s2_sar_error` | 2.46x | 11.4k | 0.0533 | 3 | 56.3x |
| `i319_pois_re2` | 2.46x | 173k | 0.813 | 5.54 | 6.81x |
| `sw_acat_cs` | 2.44x | 18.3k | 1.31 | 6.92 | 5.29x |
| `ch14_m14_3` | 2.44x | 65.2k | 0.602 | 6.13 | 10.2x |
| `s2_rate` | 2.4x | 7.8k | 0.0125 | 2.41 | 193x |
| `ch11_m11_10` | 2.39x | 10.7k | 0.0103 | 1.99 | 194x |
| `logistic_regression_rhs` | 2.39x | 137k | 6.02 | 18 | 3x |
| `Mh_model` | 2.38x | 53.8k | 0.91 | 4.18 | 4.59x |
| `ch15_m15_3` | 2.36x | 7.74k | 0.109 | 1.61 | 14.7x |
| `seeds_centered_model` | 2.35x | 41.5k | 0.0374 | 2.69 | 71.7x |
| `sw_re_bern` | 2.34x | 27.4k | 0.0283 | 3.48 | 123x |
| `dogs_hierarchical` | 2.34x | 9.37k | 0.175 | 1.87 | 10.7x |
| `s2_mv_shared_re` | 2.32x | 36.8k | 0.0669 | 6.02 | 90x |
| `ch15_m15_4` | 2.31x | 8.81k | 0.101 | 1.62 | 16x |
| `sw_skewnormal` | 2.29x | 15.5k | 0.0258 | 2.77 | 108x |
| `extra_hurdle_poisson` | 2.26x | 8.4k | 0.0084 | 1.34 | 159x |
| `i320_mi_nhanes` | 2.22x | 312k | 0.285 | 3.78 | 13.2x |
| `hier_2pl` | 2.21x | 59.4k | 11.9 | 31.8 | 2.68x |
| `sw_vonmises` | 2.21x | 730k | 0.519 | 3.61 | 6.96x |
| `normal_mixture` | 2.21x | 7.96k | 0.307 | 1.99 | 6.48x |
| `ch09_m9_4` | 2.21x | 1.55M | 0.225 | 1.79 | 7.95x |
| `ch13_m13_6` | 2.2x | 36.1k | 0.261 | 2.66 | 10.2x |
| `ch09_m9_5` | 2.19x | 398k | 0.0622 | 1.43 | 22.9x |
| `ch13_m13_4` | 2.18x | 34.4k | 0.246 | 2.67 | 10.8x |
| `lsat_model` | 2.17x | 40.8k | 1.82 | 6.38 | 3.5x |
| `ldaK2` | 2.17x | 19.8k | 0.999 | 4.6 | 4.61x |
| `ch09_m9_2` | 2.16x | 46.7k | 0.00989 | 1.27 | 129x |
| `2pl_latent_reg_irt` | 2.16x | 51.2k | 3.43 | 12.1 | 3.53x |
| `ch11_m11_5` | 2.16x | 29.3k | 0.208 | 2.27 | 10.9x |
| `sw_re_slope` | 2.16x | 53.4k | 0.0712 | 5.77 | 81x |
| `ch13_m13_4b` | 2.16x | 34.4k | 0.244 | 2.56 | 10.5x |
| `low_dim_gauss_mix` | 2.13x | 14k | 0.699 | 3.42 | 4.89x |
| `aalto_lin_std_t` | 2.13x | 13k | 0.0114 | 2.26 | 198x |
| `sw_zi_poisson` | 2.13x | 11.5k | 0.0272 | 2.17 | 79.6x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.12x | 44.4k | 2.14 | 6.39 | 2.99x |
| `s2_index_mi` | 2.11x | 18.6k | 0.028 | 3.04 | 108x |
| `ch09_m9_3` | 2.11x | 10.8k | 0.0082 | 1.27 | 155x |
| `prophet` | 2.1x | 1.66M | 61.7 | 133 | 2.15x |
| `sw_re_pois` | 2.09x | 30k | 0.034 | 3.34 | 98.2x |
| `multi_occupancy` | 2.08x | 98.9k | 3.02 | 11.1 | 3.67x |
| `dugongs_model` | 2.05x | 33.4k | 0.0232 | 1.82 | 78.6x |
| `cm_rdm` | 2.04x | 67.7k | 8.3 | 19.6 | 2.36x |
| `seeds_model` | 2.04x | 38.4k | 0.0332 | 2.48 | 74.7x |
| `cm_bisa` | 2.04x | 27.9k | 0.647 | 4.08 | 6.31x |
| `sw_cens` | 2.02x | 10.8k | 0.0243 | 3.09 | 127x |
| `irt_2pl` | 2x | 61.4k | 1.1 | 5.08 | 4.62x |
| `seeds_stanified_model` | 1.99x | 42.2k | 0.0356 | 2.38 | 67x |
| `cm_lognormal` | 1.98x | 68k | 2.68 | 8.17 | 3.05x |
| `ch13_m13_5` | 1.97x | 18.9k | 0.125 | 2.23 | 17.9x |
| `cm_lnr` | 1.97x | 41.9k | 4.75 | 12.2 | 2.57x |
| `sw_exgaussian` | 1.97x | 25.1k | 0.0325 | 2.74 | 84.2x |
| `ch11_m11_6` | 1.95x | 16.7k | 0.0156 | 1.77 | 113x |
| `normal_mixture_k` | 1.95x | 243k | 48.7 | 96.5 | 1.98x |
| `soil_incubation` | 1.93x | 203k | 4.84 | 11.5 | 2.37x |
| `ch11_m11_4` | 1.9x | 16.5k | 0.111 | 1.96 | 17.7x |
| `ch09_mp` | 1.9x | 38.9k | 0.00836 | 1.06 | 127x |
| `ch13_m13_7` | 1.89x | 19.9k | 0.00659 | 1.07 | 163x |
| `ch11_m11_8` | 1.89x | 50.9k | 0.0215 | 1.71 | 79.4x |
| `cm_loggamma` | 1.88x | 226k | 6.76 | 15.6 | 2.31x |
| `ch13_m13_1` | 1.88x | 18.5k | 0.0198 | 1.72 | 87x |
| `ch13_m13_7nc` | 1.87x | 7.88k | 0.00689 | 1.1 | 160x |
| `ch13_m13_2` | 1.87x | 22.9k | 0.0247 | 1.89 | 76.4x |
| `lotka_volterra` | 1.86x | 75.1k | 2.52 | 7.75 | 3.08x |
| `ch13_m13_3` | 1.84x | 27.8k | 0.0314 | 1.81 | 57.8x |
| `s2_cens_interval` | 1.84x | 8.94k | 0.0241 | 2.59 | 107x |
| `sw_arma` | 1.84x | 42.2k | 0.104 | 3.31 | 31.7x |
| `i319_pois_re` | 1.84x | 192k | 0.711 | 4.55 | 6.4x |
| `sw_ar` | 1.8x | 10.4k | 0.0275 | 3.09 | 112x |
| `sw_hurdle_gamma` | 1.8x | 13.3k | 0.0394 | 2.37 | 60.3x |
| `sw_ma` | 1.79x | 11.1k | 0.0296 | 3.06 | 104x |
| `ch11_m11_11` | 1.76x | 143k | 0.0608 | 2.41 | 39.6x |
| `sw_mi` | 1.75x | 15.1k | 0.0203 | 2.63 | 130x |
| `cm_logweibull` | 1.75x | 90.4k | 1.86 | 6.07 | 3.26x |
| `sw_zi_binomial` | 1.75x | 10.2k | 0.0341 | 2.22 | 65.2x |
| `wells_dist` | 1.73x | 26.5k | 0.657 | 2.79 | 4.24x |
| `cm_choco` | 1.72x | 15.1k | 0.686 | 4.38 | 6.38x |
| `s2_threading` | 1.72x | 11.8k | 0.0129 | 2.17 | 168x |
| `cm_weibull` | 1.72x | 2.03M | 37 | 66.4 | 1.79x |
| `sw_beta` | 1.68x | 8.99k | 0.0251 | 2.69 | 107x |
| `cm_logstudent` | 1.68x | 2.02M | 30.7 | 54.5 | 1.78x |
| `cm_lba1` | 1.67x | 145k | 7.91 | 16.2 | 2.05x |
| `s2_mv_subset` | 1.66x | 12.1k | 0.0163 | 2.26 | 139x |
| `cm_gamma` | 1.66x | 2.01M | 41.2 | 71.3 | 1.73x |
| `sw_gaussian` | 1.65x | 10.9k | 0.0116 | 2.08 | 180x |
| `sw_asymlaplace` | 1.65x | 23.4k | 0.0578 | 2.41 | 41.6x |
| `s2_zoi_beta` | 1.6x | 12.1k | 0.062 | 2.53 | 40.8x |
| `ch11_m11_7` | 1.59x | 7.03k | 0.00843 | 1.6 | 190x |
| `surgical_model` | 1.59x | 22.2k | 0.0165 | 2.19 | 133x |
| `s2_zi_beta` | 1.58x | 10.2k | 0.0533 | 2.49 | 46.6x |
| `sw_mv_norescor` | 1.58x | 11.6k | 0.0176 | 2.27 | 129x |
| `cm_betagate` | 1.58x | 11.8k | 0.422 | 3.38 | 8x |
| `cm_lnr_bench` | 1.57x | 40.1k | 10.7 | 20.1 | 1.87x |
| `GLM_Binomial_model` | 1.57x | 13.3k | 0.019 | 2.06 | 109x |
| `sir` | 1.57x | 127k | 20.5 | 35.1 | 1.71x |
| `cm_invweibull` | 1.56x | 87.1k | 2.02 | 5.93 | 2.93x |
| `s2_unstr` | 1.54x | 46.4k | 0.219 | 6.64 | 30.3x |
| `cm_invgamma` | 1.52x | 79k | 1.8 | 5.55 | 3.07x |
| `s2_frechet` | 1.52x | 33.2k | 0.073 | 2.86 | 39.2x |
| `Rate_4_model` | 1.52x | 8.3k | 0.00767 | 1.27 | 165x |
| `gp_pois_regr` | 1.51x | 396k | 0.754 | 5.65 | 7.49x |
| `s2_categorical_re` | 1.51x | 34.6k | 0.185 | 4.48 | 24.3x |
| `gp_regr` | 1.5x | 8.82k | 0.028 | 4.28 | 153x |
| `sw_re_negbin` | 1.5x | 30.3k | 0.0661 | 3.61 | 54.6x |
| `sw_weibull` | 1.49x | 10.3k | 0.0287 | 2.7 | 94.3x |
| `hmm_example` | 1.48x | 25.1k | 0.494 | 3.54 | 7.17x |
| `sw_zi_negbin` | 1.48x | 20.5k | 0.0747 | 2.33 | 31.1x |
| `cm_lba2` | 1.48x | 140k | 13.7 | 23.6 | 1.71x |
| `covid19imperial_v2` | 1.45x | 473k | 119 | 176 | 1.48x |
| `covid19imperial_v3` | 1.45x | 473k | 119 | 177 | 1.49x |
| `aalto_bern` | 1.45x | 5.11k | 0.00672 | 1.28 | 191x |
| `s2_gp_by_gr` | 1.44x | 59.6k | 0.434 | 8.44 | 19.4x |
| `s2_mixture_theta` | 1.44x | 80.4k | 0.488 | 4 | 8.21x |
| `Mb_model` | 1.43x | 11k | 0.433 | 2.58 | 5.97x |
| `s2_com_poisson` | 1.43x | 42.5k | 0.898 | 3.66 | 4.07x |
| `aalto_binom` | 1.42x | 5.11k | 0.00652 | 1.14 | 175x |
| `ch14_m14_2` | 1.42x | 135k | 2.25 | 9.06 | 4.02x |
| `s2_zi_asymlaplace` | 1.42x | 28.3k | 0.0971 | 2.5 | 25.7x |
| `i320_gp_expquad` | 1.4x | 52.4k | 0.572 | 6.63 | 11.6x |
| `ch15_m15_7` | 1.4x | 28k | 0.264 | 6.34 | 24x |
| `Rate_2_model` | 1.39x | 7.31k | 0.00738 | 1.28 | 173x |
| `ch12_m12_6` | 1.38x | 111k | 222 | 311 | 1.4x |
| `aalto_binom2` | 1.38x | 7.7k | 0.00766 | 1.25 | 163x |
| `Rate_1_model` | 1.37x | 4.69k | 0.00585 | 1.13 | 193x |
| `ch11_m11_9` | 1.35x | 5.08k | 0.00758 | 1.34 | 176x |
| `Rate_5_model` | 1.35x | 5.33k | 0.00744 | 1.24 | 166x |
| `sw_binomial` | 1.35x | 7.52k | 0.0224 | 2.36 | 106x |
| `ch14_m14_8nc` | 1.34x | 263k | 0.734 | 5.41 | 7.37x |
| `i319_negbin_re` | 1.33x | 66.7k | 0.656 | 4.4 | 6.71x |
| `aalto_binomb` | 1.33x | 5.61k | 0.00699 | 1.17 | 168x |
| `Rate_3_model` | 1.33x | 5.2k | 0.00726 | 1.16 | 160x |
| `ch12_m12_2` | 1.32x | 64.2k | 0.064 | 2.83 | 44.1x |
| `sw_mono` | 1.32x | 20.4k | 0.044 | 2.78 | 63x |
| `ch12_m12_1` | 1.31x | 11.8k | 0.0196 | 1.99 | 102x |
| `s2_beta_binomial` | 1.31x | 10.4k | 0.0521 | 2.53 | 48.5x |
| `s2_mo_simo_prior` | 1.31x | 21.3k | 0.0476 | 2.8 | 58.9x |
| `ch14_m14_1` | 1.3x | 37.6k | 0.205 | 5.98 | 29.2x |
| `ch14_m14_6` | 1.29x | 21.6k | 1.87 | 7.84 | 4.18x |
| `sw_gp` | 1.27x | 47.3k | 0.92 | 7.02 | 7.63x |
| `ch14_m14_6x` | 1.26x | 21.6k | 1.89 | 7.78 | 4.12x |
| `ch11_m_pois` | 1.26x | 6.85k | 0.00732 | 1.26 | 172x |
| `i320_sratio_plain` | 1.25x | 15.4k | 0.924 | 3.68 | 3.99x |
| `nn_rbm1bJ10` | 1.25x | 2.03M | 330 | 415 | 1.26x |
| `ch14_m14_9` | 1.25x | 12.5k | 4.37 | 8.26 | 1.89x |
| `ch14_m14_10` | 1.24x | 11.2k | 4.15 | 8.02 | 1.93x |
| `s2_custom_vint` | 1.24x | 10.5k | 0.0599 | 2.39 | 40x |
| `sw_sratio` | 1.23x | 10.4k | 0.631 | 3.15 | 4.99x |
| `sw_bernoulli` | 1.22x | 8.63k | 0.0117 | 2.03 | 173x |
| `ch14_m14_8` | 1.22x | 204k | 0.577 | 3.97 | 6.88x |
| `sw_mv_rescor` | 1.21x | 13.3k | 0.0896 | 5.12 | 57.1x |
| `sw_mixture` | 1.21x | 303k | 1.7 | 5.6 | 3.29x |
| `s2_cumulative_probit` | 1.2x | 13.7k | 0.0665 | 2.56 | 38.5x |
| `sw_cratio` | 1.19x | 10.4k | 0.631 | 3.13 | 4.95x |
| `ch14_m14_7` | 1.17x | 143k | 4.11 | 12.4 | 3.02x |
| `sw_poisson` | 1.16x | 9.85k | 0.0131 | 1.87 | 143x |
| `sw_cratio_cs` | 1.16x | 13.7k | 1.24 | 4.93 | 3.99x |
| `i320_sratio_cs` | 1.15x | 20.4k | 1.83 | 5.86 | 3.2x |
| `sw_categorical` | 1.15x | 11k | 0.0219 | 3.13 | 143x |
| `s2_multinomial` | 1.14x | 12.9k | 0.129 | 2.91 | 22.6x |
| `Survey_model` | 1.13x | 11.1k | 0.65 | 2.44 | 3.75x |
| `s2_discrete_weibull` | 1.12x | 31.8k | 0.12 | 2.37 | 19.7x |
| `nn_rbm1bJ100` † | 1.08x | 5k | 2.19e+03 | 2.36e+03 | 1.08x |
| `ch14_m14_11` | 1.07x | 12.7k | 8.01 | 11.3 | 1.41x |
| `sw_negbinomial` | 1.06x | 1.49M | 2.09 | 4.36 | 2.09x |
| `s2_weights_trunc` | 1.06x | 25.1k | 0.173 | 2.58 | 14.9x |
| `s2_hurdle_negbin` | 1.06x | 15.2k | 0.114 | 2.34 | 20.6x |
| `sw_trunc` | 1.05x | 27.2k | 0.181 | 2.54 | 14.1x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0738 | 1.95 | 26.4x |
| `i319_negbin_fixed` | 1.04x | 19.4k | 0.186 | 2.35 | 12.6x |
| `wells_dist100_model` | 1.04x | 11.5k | 0.204 | 2.16 | 10.6x |
| `s2_dirichlet` | 1.04x | 12.6k | 0.169 | 3.31 | 19.5x |
| `hmm_gaussian` | 1.03x | 63.9k | 17.8 | 21.9 | 1.23x |
| `nes_logit_model` | 1.03x | 17.8k | 0.126 | 2.04 | 16.1x |
| `bones_model` | 1.03x | 14.7k | 0.782 | 3.02 | 3.86x |
| `wells_dae_model` | 1.03x | 24.2k | 0.502 | 2.51 | 5x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.327 | 2.44 | 7.47x |
| `wells_daae_c_model` | 1.02x | 17.6k | 0.382 | 2.53 | 6.61x |
| `aalto_poisson_simple` | 1.02x | 5.02k | 0.0124 | 1.4 | 113x |
| `i320_pois_trunc_ub` | 1.02x | 21.9k | 0.572 | 2.83 | 4.95x |
| `wells_dist100ars_model` | 1.02x | 19.1k | 0.366 | 2.35 | 6.42x |
| `iohmm_reg` | 1.02x | 462k | 157 | 164 | 1.04x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.02x | 11.2k | 0.236 | 2.32 | 9.8x |
| `wells_dae_inter_model` | 1.01x | 12.8k | 0.284 | 2.41 | 8.48x |
| `hmm_drive_1` | 1.01x | 39.6k | 6.1 | 9.66 | 1.58x |
| `diamonds` | 1.01x | 1.51M | 49.2 | 52.4 | 1.06x |
| `wells_interaction_model` | 1.01x | 32.1k | 0.672 | 2.71 | 4.04x |
| `s2_ar_cov` | 1x | 11.2k | 0.0868 | 6.08 | 70.1x |
| `s2_wiener` | 1x | 14.7k | 0.368 | 2.8 | 7.61x |
| `one_comp_mm_elim_abs` | 1x | 18.9k | 8.86 | 11 | 1.24x |
| `cm_betadiscrete` | 1x | 9.91k | 23 | 26.1 | 1.14x |
| `garch11` | 0.999x | 23.3k | 0.206 | 1.9 | 9.2x |
| `sw_cumulative_cs` | 0.992x | 22.3k | 1.45 | 4.94 | 3.42x |
| `arma11` | 0.986x | 11.2k | 0.0711 | 1.78 | 25.1x |
| `i320_pois_trunc_both` | 0.985x | 21.9k | 0.727 | 3.03 | 4.16x |
| `s2_mi_trunc_lb` | 0.98x | 37.9k | 0.217 | 3.45 | 15.9x |
| `s2_cosy` | 0.975x | 14.7k | 0.104 | 5.38 | 51.9x |
| `cm_ddm` † | 0.975x | 5k | 841 | 825 | 0.982x |
| `sw_cumulative` | 0.973x | 9.89k | 0.228 | 2.76 | 12.1x |
| `s2_fcor` | 0.958x | 9.87k | 0.146 | 3.86 | 26.5x |
| `i320_gp_matern32` | 0.953x | 133k | 4.13 | 9.63 | 2.33x |
| `hmm_drive_0` | 0.948x | 24.5k | 3.72 | 7.04 | 1.89x |
| `cm_invgaussian` † | 0.756x | 5k | 0.873 | 3.99 | 4.57x |
| `aalto_gpareto` | 0.755x | 9.37k | 0.016 | 1.63 | 102x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling | Reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `kronecker_gp` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.0723 |
<!--/gen-->

### CmdStan with stanc3 loop vectorization

This table repeats the comparison with stanc3's loop vectorization turned on
for CmdStan at `--O1`. The Stanli timings are the default run's.
[How this table is built](#loop-vectorization-comparison) is in the appendix.

<!--gen:benchmark_vectorized_catalog-->
Stanli timings are from the default run `144302fd2f39cf47`. The CmdStan timings with loop vectorization are reused from run `7103c79930334f45` (2026-10-21). Each ratio is CmdStan's median gradient time divided by Stanli's, each averaged over 10 evaluation points.

CmdStan stanc and build times for 6 of 343 models were reused from the build cache and not remeasured in the runs that report them.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 32.4x | 18.6k | 1.29 | 43.2 | 33.4x |
| `gpcm_latent_reg_irt` | 12.9x | 69k | 7.97 | 106 | 13.3x |
| `grsm_latent_reg_irt` | 12x | 63.1k | 4.23 | 54.5 | 12.9x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.3x | 47.3k | 1.26 | 15.6 | 12.4x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.7x | 68.5k | 1.87 | 21.6 | 11.6x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.32x | 50.6k | 2.3 | 20.6 | 8.92x |
| `radon_county_intercept` | 8.17x | 62.8k | 2.85 | 24.5 | 8.62x |
| `aalto_poisson_hurdle` | 8.13x | 6.92k | 0.0783 | 2.11 | 27x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8x | 52.9k | 2.47 | 21.2 | 8.56x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8x | 81.2k | 3.78 | 31.8 | 8.41x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.68x | 102k | 4.85 | 38.9 | 8.02x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.51x | 125k | 6.5 | 50.6 | 7.78x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.46x | 59.7k | 3.11 | 24.7 | 7.94x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.44x | 77.7k | 5.12 | 39.4 | 7.7x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.42x | 100k | 6.58 | 50.4 | 7.66x |
| `s2_custom_vreal` | 7.33x | 10.2k | 0.0122 | 2.25 | 184x |
| `ch16_m16_1` | 6.74x | 301k | 1.23 | 9.95 | 8.09x |
| `rats_model` | 6.72x | 54.5k | 0.0484 | 2.09 | 43.1x |
| `ch12_m12_3` | 6.65x | 10.8k | 0.0395 | 1.46 | 36.9x |
| `M0_model` | 6.63x | 7.46k | 0.028 | 1.55 | 55.4x |
| `ch12_m12_3_alt` | 6.62x | 11k | 0.0387 | 1.45 | 37.5x |
| `ctsem_ctsm_400` † | 5.51x | 5k | 20.4 | 550 | 27x |
| `ctsem_ctsm` | 5.46x | 1.08M | 438 | 2.83e+03 | 6.46x |
| `Mt_model` | 5.1x | 11k | 0.0503 | 2.43 | 48.2x |
| `arK` | 4.87x | 59.1k | 0.11 | 2.09 | 19.1x |
| `s2_invgaussian` | 4.58x | 16.3k | 0.0147 | 2.47 | 168x |
| `Mth_model` | 4.1x | 33.9k | 0.89 | 6.62 | 7.44x |
| `s2_car_icar` | 3.91x | 1.7M | 0.744 | 6.04 | 8.11x |
| `s2_cumulative_cauchit` | 3.87x | 20.7k | 0.0311 | 2.6 | 83.5x |
| `s2_logistic_normal` | 3.72x | 13.6k | 0.0836 | 5.79 | 69.2x |
| `sw_weights` | 3.48x | 9.98k | 0.014 | 2.25 | 160x |
| `dogs_log` | 3.46x | 12.1k | 0.128 | 2.95 | 23.1x |
| `s2_mm` | 3.45x | 26.6k | 0.0281 | 3.54 | 126x |
| `s2_mm_weights` | 3.42x | 27.8k | 0.0289 | 3.54 | 122x |
| `dogs` | 3.42x | 20.7k | 0.205 | 3.54 | 17.2x |
| `s2_cox_cens` | 3.42x | 22.1k | 0.0314 | 3.82 | 122x |
| `sw_me` | 3.39x | 57.9k | 0.0408 | 3.12 | 76.5x |
| `s2_gp_by_approx` | 3.29x | 37.2k | 0.0552 | 5.42 | 98.2x |
| `i319_gauss_re` | 3.28x | 41.3k | 0.0715 | 3.58 | 50x |
| `cm_exgaussian` | 3.23x | 14k | 0.222 | 3.74 | 16.9x |
| `GLMM1_model` | 3.23x | 32.8k | 0.333 | 3 | 9.02x |
| `s2_me2_nomecor` | 3.22x | 50.4k | 0.0571 | 3.46 | 60.6x |
| `cm_exwald` | 3.21x | 68k | 3.03 | 14.2 | 4.69x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.18x | 1.03M | 6.38 | 23.9 | 3.74x |
| `s2_car` | 3.16x | 30.6k | 0.032 | 3.55 | 111x |
| `s2_car_esicar` | 3.15x | 28.9k | 0.0289 | 3.59 | 124x |
| `s2_gr_by` | 3.14x | 33.2k | 0.0278 | 3.77 | 135x |
| `s2_dist_sigma_re` | 3.14x | 29.4k | 0.0311 | 3.8 | 122x |
| `ch09_m9_1_chains4` | 3.09x | 11.3k | 0.0148 | 2.07 | 140x |
| `mesquite` | 3.06x | 160k | 0.0794 | 2.32 | 29.2x |
| `ch09_m9_1` | 3.05x | 11.3k | 0.015 | 2.07 | 138x |
| `s2_cox` | 3.04x | 30.6k | 0.0347 | 2.92 | 84.3x |
| `logmesquite_logva` | 3.03x | 58.7k | 0.0244 | 2.12 | 86.9x |
| `logmesquite_logvash` | 3.03x | 93.7k | 0.045 | 2.4 | 53.4x |
| `logmesquite_logvas` | 3.01x | 92.3k | 0.0502 | 2.53 | 50.3x |
| `sw_re_gauss` | 3x | 29.1k | 0.0241 | 3.41 | 142x |
| `logmesquite` | 3x | 56.4k | 0.0343 | 2.23 | 64.9x |
| `radon_county` | 3x | 48.8k | 1.28 | 5.66 | 4.42x |
| `aalto_grp_aov` | 2.98x | 12.5k | 0.00812 | 1.78 | 219x |
| `logmesquite_logvolume` | 2.98x | 12.4k | 0.0101 | 1.79 | 177x |
| `s2_hurdle_cumulative` | 2.95x | 11.2k | 0.0408 | 3.01 | 73.9x |
| `ch15_m15_1` | 2.87x | 30.7k | 0.0194 | 2.2 | 113x |
| `s2_gr_student` | 2.82x | 52.2k | 0.0453 | 4.2 | 92.6x |
| `dogs_nonhierarchical` | 2.79x | 56.4k | 0.89 | 8.05 | 9.04x |
| `cm_geg` | 2.76x | 54k | 1.37 | 7.04 | 5.14x |
| `ch15_m15_8` | 2.72x | 10.7k | 0.0321 | 1.67 | 52x |
| `Mtbh_model` | 2.69x | 34.3k | 0.566 | 5.12 | 9.05x |
| `ch15_m15_5` | 2.69x | 21.5k | 0.017 | 2.46 | 144x |
| `sw_acat_cs` | 2.66x | 18.3k | 1.31 | 7.27 | 5.55x |
| `ch15_m15_9` | 2.66x | 11.3k | 0.0349 | 1.93 | 55.2x |
| `sw_hurdle_lognormal` | 2.65x | 12.5k | 0.0257 | 2.37 | 92.2x |
| `kilpisjarvi` | 2.64x | 1.12M | 0.233 | 2.12 | 9.1x |
| `eight_schools_centered` | 2.63x | 32.4k | 0.0103 | 1.73 | 168x |
| `aalto_grp_prior_mean` | 2.62x | 19.3k | 0.0102 | 1.87 | 183x |
| `ch09_m5_8s` | 2.62x | 1.37M | 0.629 | 3.39 | 5.4x |
| `hierarchical_gp` | 2.6x | 352k | 6.01 | 23.3 | 3.89x |
| `sw_hurdle_pois` | 2.59x | 10.4k | 0.0293 | 2.23 | 76.1x |
| `s2_mmc` | 2.59x | 35.4k | 0.0629 | 5.98 | 95x |
| `sw_acat` | 2.58x | 12.2k | 0.63 | 4.3 | 6.83x |
| `ch09_m5_8s2` | 2.57x | 1.11M | 0.53 | 3.13 | 5.9x |
| `ch15_m15_2` | 2.56x | 32.4k | 0.0267 | 2.43 | 90.9x |
| `s2_shifted_lognormal` | 2.51x | 15.1k | 0.0161 | 2.72 | 169x |
| `ldaK5` † | 2.51x | 5k | 12.6 | 32.9 | 2.61x |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.5x | 17.3k | 0.00905 | 1.99 | 220x |
| `s2_s_cc` | 2.5x | 30.5k | 0.0226 | 3.05 | 135x |
| `aalto_grp_prior_mean_var` | 2.47x | 59.3k | 0.0247 | 2.64 | 107x |
| `s2_nlf` | 2.47x | 9.42k | 0.0168 | 3.14 | 187x |
| `nes` | 2.43x | 90.8k | 1.69 | 6.44 | 3.81x |
| `pilots` | 2.43x | 551k | 0.194 | 2.7 | 13.9x |
| `ch12_m12_5` | 2.43x | 37.4k | 39.1 | 97.1 | 2.48x |
| `s2_gp_approx` | 2.42x | 34k | 0.0327 | 4.33 | 133x |
| `ch12_m12_7` | 2.42x | 48.6k | 50.3 | 124 | 2.46x |
| `s2_me2` | 2.42x | 71.6k | 0.131 | 5.83 | 44.4x |
| `aalto_lin_std` | 2.41x | 11k | 0.00954 | 2 | 210x |
| `i319_pois_re2` | 2.39x | 173k | 0.813 | 5.52 | 6.79x |
| `ch14_m14_3` | 2.38x | 65.2k | 0.602 | 6.08 | 10.1x |
| `aalto_lin` | 2.38x | 18.2k | 0.0098 | 1.77 | 181x |
| `cm_lnr` | 2.37x | 41.9k | 4.75 | 23.3 | 4.9x |
| `sw_spline_s` | 2.36x | 37.6k | 0.0283 | 3.19 | 113x |
| `accel_gp` | 2.36x | 1.5M | 5.3 | 17.8 | 3.35x |
| `normal_mixture` | 2.34x | 7.96k | 0.307 | 2.25 | 7.33x |
| `kidscore_interaction` | 2.33x | 178k | 0.473 | 3.02 | 6.38x |
| `kidscore_interaction_c` | 2.33x | 15.2k | 0.0467 | 2.04 | 43.6x |
| `dogs_hierarchical` | 2.33x | 9.37k | 0.175 | 1.88 | 10.7x |
| `s2_mv_shared_re` | 2.32x | 36.8k | 0.0669 | 6.09 | 91.1x |
| `sw_re_bern` | 2.32x | 27.4k | 0.0283 | 3.48 | 123x |
| `s2_gev` | 2.29x | 12.2k | 0.0357 | 2.67 | 74.8x |
| `kidscore_interaction_z` | 2.29x | 24k | 0.0707 | 2.16 | 30.6x |
| `ch15_m15_6` | 2.29x | 15.6k | 0.0115 | 1.91 | 166x |
| `i320_mi_nhanes` | 2.28x | 312k | 0.285 | 3.85 | 13.5x |
| `s2_cumulative_cloglog` | 2.27x | 20.3k | 0.0342 | 2.61 | 76.5x |
| `sw_re_slope` | 2.27x | 53.4k | 0.0712 | 5.84 | 81.9x |
| `kidscore_interaction_c2` | 2.27x | 17.7k | 0.0542 | 2.02 | 37.2x |
| `kidscore_mom_work` | 2.26x | 40.5k | 0.114 | 2.07 | 18.1x |
| `losscurve_sislob` | 2.25x | 21.3k | 0.0331 | 3.5 | 106x |
| `ch11_m11_5` | 2.24x | 29.3k | 0.208 | 2.27 | 10.9x |
| `extra_hurdle_poisson` | 2.24x | 8.4k | 0.0084 | 1.33 | 158x |
| `Mh_model` | 2.24x | 53.8k | 0.91 | 4.03 | 4.42x |
| `sw_skewnormal` | 2.23x | 15.5k | 0.0258 | 2.72 | 106x |
| `sw_vonmises` | 2.23x | 730k | 0.519 | 3.58 | 6.9x |
| `s2_sar_error` | 2.23x | 11.4k | 0.0533 | 3.03 | 56.9x |
| `s2_sar` | 2.22x | 10.9k | 0.0491 | 3.08 | 62.8x |
| `cm_rdm` | 2.21x | 67.7k | 8.3 | 27.4 | 3.3x |
| `sw_nonlinear` | 2.21x | 10.9k | 0.0141 | 2.66 | 189x |
| `logearn_logheight_male` | 2.2x | 715k | 4.01 | 10.6 | 2.65x |
| `s2_mi_lognormal` | 2.2x | 14.8k | 0.0217 | 3.34 | 154x |
| `logearn_height_male` | 2.18x | 189k | 1.08 | 4.14 | 3.83x |
| `ch13_m13_4b` | 2.18x | 34.4k | 0.244 | 2.58 | 10.6x |
| `ch09_m9_4` | 2.18x | 1.55M | 0.225 | 1.78 | 7.9x |
| `ch09_m9_5` | 2.18x | 398k | 0.0622 | 1.43 | 22.9x |
| `ch14_m14_5` | 2.16x | 12k | 0.0357 | 1.82 | 50.8x |
| `ch13_m13_4` | 2.16x | 34.4k | 0.246 | 2.68 | 10.9x |
| `GLMM_Poisson_model` | 2.15x | 75.6k | 0.0529 | 2.69 | 50.9x |
| `ch13_m13_6` | 2.15x | 36.1k | 0.261 | 2.67 | 10.2x |
| `sw_re_pois` | 2.14x | 30k | 0.034 | 3.35 | 98.5x |
| `logearn_interaction_z` | 2.14x | 22k | 0.182 | 2.39 | 13.1x |
| `kidscore_momhsiq` | 2.14x | 90.5k | 0.189 | 2.19 | 11.6x |
| `sw_lognormal` | 2.13x | 8.28k | 0.0127 | 2.42 | 190x |
| `logearn_interaction` | 2.13x | 322k | 2.56 | 7.36 | 2.87x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.13x | 44.4k | 2.14 | 6.37 | 2.98x |
| `ch13_m13_4nc` | 2.12x | 75.5k | 0.678 | 3.48 | 5.13x |
| `low_dim_gauss_mix` | 2.11x | 14k | 0.699 | 3.43 | 4.91x |
| `ch09_m9_3` | 2.11x | 10.8k | 0.0082 | 1.26 | 154x |
| `s2_s_by` | 2.1x | 70.7k | 0.119 | 4.7 | 39.6x |
| `ch09_m9_2` | 2.1x | 46.7k | 0.00989 | 1.26 | 127x |
| `earn_height` | 2.1x | 150k | 0.562 | 2.76 | 4.92x |
| `log10earn_height` | 2.08x | 143k | 0.532 | 2.69 | 5.06x |
| `sesame_one_pred_a` | 2.07x | 15.5k | 0.0197 | 1.61 | 81.8x |
| `accel_splines` | 2.07x | 1.86M | 7.07 | 17.9 | 2.54x |
| `logearn_height` | 2.06x | 138k | 0.526 | 2.71 | 5.14x |
| `ldaK2` | 2.06x | 19.8k | 0.999 | 4.44 | 4.44x |
| `cm_lognormal` | 2.06x | 68k | 2.68 | 9.67 | 3.61x |
| `dugongs_model` | 2.04x | 33.4k | 0.0232 | 1.8 | 77.5x |
| `seeds_centered_model` | 2.03x | 41.5k | 0.0374 | 2.71 | 72.4x |
| `radon_pooled` | 2.03x | 9.33k | 0.401 | 2.4 | 5.97x |
| `kidscore_momhs` | 2.02x | 24.8k | 0.0418 | 1.72 | 41.2x |
| `ch14_m14_4` | 2.01x | 9.05k | 0.0219 | 1.64 | 74.7x |
| `ch14_m14_4x` | 2.01x | 9.05k | 0.0222 | 1.64 | 73.7x |
| `GLM_Poisson_model` | 2.01x | 29.5k | 0.0188 | 2.06 | 110x |
| `kidscore_momiq` | 2.01x | 60.1k | 0.0908 | 1.82 | 20.1x |
| `sw_zi_poisson` | 2.01x | 11.5k | 0.0272 | 2.22 | 81.5x |
| `ch11_m11_10` | 2.01x | 10.7k | 0.0103 | 2 | 194x |
| `s2_nl_noloop` | 2.01x | 52.6k | 0.0346 | 2.63 | 75.9x |
| `aalto_lin_std_t` | 2x | 13k | 0.0114 | 2.21 | 194x |
| `sw_spline_t2` | 1.97x | 38.1k | 0.0461 | 3.65 | 79.2x |
| `sw_student` | 1.96x | 12.7k | 0.0158 | 2.58 | 163x |
| `normal_mixture_k` | 1.96x | 243k | 48.7 | 97.3 | 2x |
| `ch11_m11_6` | 1.95x | 16.7k | 0.0156 | 1.78 | 114x |
| `election88_full` | 1.94x | 429k | 103 | 203 | 1.98x |
| `ch13_m13_5` | 1.94x | 18.9k | 0.125 | 2.22 | 17.8x |
| `sw_se` | 1.94x | 7.2k | 0.0119 | 2.22 | 187x |
| `cm_bisa` | 1.94x | 27.9k | 0.647 | 4.15 | 6.42x |
| `s2_index_mi` | 1.94x | 18.6k | 0.028 | 2.98 | 107x |
| `ch11_m11_4` | 1.93x | 16.5k | 0.111 | 1.95 | 17.6x |
| `ch13_m13_1` | 1.93x | 18.5k | 0.0198 | 1.73 | 87.7x |
| `sw_dist_sigma` | 1.92x | 10.7k | 0.0167 | 2.56 | 153x |
| `cm_loggamma` | 1.91x | 226k | 6.76 | 16.3 | 2.41x |
| `ch16_m16_4` | 1.91x | 35.3k | 0.0639 | 2.19 | 34.2x |
| `multi_occupancy` | 1.89x | 98.9k | 3.02 | 11 | 3.63x |
| `cm_lnr_bench` | 1.89x | 40.1k | 10.7 | 27 | 2.51x |
| `ch13_m13_7nc` | 1.88x | 7.88k | 0.00689 | 1.11 | 161x |
| `s2_t2_by` | 1.87x | 36.4k | 0.0732 | 4.56 | 62.3x |
| `ch13_m13_2` | 1.87x | 22.9k | 0.0247 | 1.9 | 76.9x |
| `ch11_m11_8` | 1.87x | 50.9k | 0.0215 | 1.69 | 78.7x |
| `ch09_mp` | 1.86x | 38.9k | 0.00836 | 1.07 | 128x |
| `ch13_m13_7` | 1.85x | 19.9k | 0.00659 | 1.05 | 159x |
| `i319_pois_re` | 1.84x | 192k | 0.711 | 4.61 | 6.48x |
| `ch13_m13_3` | 1.84x | 27.8k | 0.0314 | 1.81 | 57.8x |
| `s2_cens_interval` | 1.83x | 8.94k | 0.0241 | 2.55 | 106x |
| `ch15_m15_3` | 1.8x | 7.74k | 0.109 | 1.57 | 14.4x |
| `soil_incubation` | 1.8x | 203k | 4.84 | 11.3 | 2.34x |
| `lotka_volterra` | 1.79x | 75.1k | 2.52 | 7.71 | 3.06x |
| `cm_lba1` | 1.78x | 145k | 7.91 | 17.8 | 2.24x |
| `ch11_m11_11` | 1.77x | 143k | 0.0608 | 2.41 | 39.6x |
| `cm_logweibull` | 1.76x | 90.4k | 1.86 | 6.07 | 3.26x |
| `sw_asymlaplace` | 1.76x | 23.4k | 0.0578 | 2.48 | 42.8x |
| `sw_mi` | 1.75x | 15.1k | 0.0203 | 2.64 | 130x |
| `sw_hurdle_gamma` | 1.73x | 13.3k | 0.0394 | 2.35 | 59.7x |
| `bym2_offset_only` | 1.72x | 202k | 7.66 | 16.3 | 2.12x |
| `cm_weibull` | 1.71x | 2.03M | 37 | 66.2 | 1.79x |
| `hier_2pl` | 1.71x | 59.4k | 11.9 | 25.9 | 2.18x |
| `cm_logstudent` | 1.69x | 2.02M | 30.7 | 54.7 | 1.78x |
| `ch15_m15_4` | 1.68x | 8.81k | 0.101 | 1.53 | 15.1x |
| `s2_threading` | 1.66x | 11.8k | 0.0129 | 2.18 | 169x |
| `sw_zi_binomial` | 1.65x | 10.2k | 0.0341 | 2.24 | 65.8x |
| `cm_gamma` | 1.65x | 2.01M | 41.2 | 70.7 | 1.72x |
| `2pl_latent_reg_irt` | 1.65x | 51.2k | 3.43 | 9.83 | 2.87x |
| `seeds_stanified_model` | 1.65x | 42.2k | 0.0356 | 2.43 | 68.3x |
| `cm_betagate` | 1.64x | 11.8k | 0.422 | 3.63 | 8.59x |
| `sw_arma` | 1.64x | 42.2k | 0.104 | 3.26 | 31.2x |
| `seeds_model` | 1.63x | 38.4k | 0.0332 | 2.53 | 76.1x |
| `sw_ar` | 1.62x | 10.4k | 0.0275 | 3.04 | 111x |
| `sw_ma` | 1.61x | 11.1k | 0.0296 | 3.03 | 103x |
| `ch11_m11_7` | 1.6x | 7.03k | 0.00843 | 1.59 | 188x |
| `sw_cens` | 1.6x | 10.8k | 0.0243 | 3.02 | 125x |
| `sw_gaussian` | 1.59x | 10.9k | 0.0116 | 2.1 | 181x |
| `cm_lba2` | 1.59x | 140k | 13.7 | 27.9 | 2.03x |
| `cm_invweibull` | 1.59x | 87.1k | 2.02 | 6.01 | 2.97x |
| `s2_mv_subset` | 1.58x | 12.1k | 0.0163 | 2.28 | 139x |
| `sir` | 1.56x | 127k | 20.5 | 35 | 1.7x |
| `surgical_model` | 1.55x | 22.2k | 0.0165 | 2.2 | 134x |
| `cm_choco` | 1.55x | 15.1k | 0.686 | 4.84 | 7.06x |
| `sw_mv_norescor` | 1.54x | 11.6k | 0.0176 | 2.27 | 129x |
| `s2_unstr` | 1.54x | 46.4k | 0.219 | 7.82 | 35.7x |
| `cm_invgamma` | 1.54x | 79k | 1.8 | 5.59 | 3.09x |
| `sw_cratio_cs` | 1.52x | 13.7k | 1.24 | 5.66 | 4.58x |
| `sw_re_negbin` | 1.52x | 30.3k | 0.0661 | 3.65 | 55.2x |
| `sw_gamma` | 1.52x | 9.45k | 0.0143 | 2.55 | 178x |
| `Rate_4_model` | 1.51x | 8.3k | 0.00767 | 1.26 | 165x |
| `s2_categorical_re` | 1.49x | 34.6k | 0.185 | 4.46 | 24.1x |
| `Mb_model` | 1.49x | 11k | 0.433 | 2.61 | 6.03x |
| `i320_sratio_cs` | 1.48x | 20.4k | 1.83 | 6.77 | 3.69x |
| `lsat_model` | 1.48x | 40.8k | 1.82 | 5.17 | 2.84x |
| `hmm_example` | 1.47x | 25.1k | 0.494 | 3.54 | 7.16x |
| `gp_regr` | 1.46x | 8.82k | 0.028 | 4.23 | 151x |
| `prophet` | 1.45x | 1.66M | 61.7 | 94.1 | 1.52x |
| `s2_rate` | 1.45x | 7.8k | 0.0125 | 2.37 | 189x |
| `s2_gp_by_gr` | 1.44x | 59.6k | 0.434 | 9.46 | 21.8x |
| `sw_zi_negbin` | 1.44x | 20.5k | 0.0747 | 2.33 | 31.2x |
| `gp_pois_regr` | 1.44x | 396k | 0.754 | 5.68 | 7.53x |
| `s2_com_poisson` | 1.44x | 42.5k | 0.898 | 4.7 | 5.24x |
| `ch14_m14_2` | 1.44x | 135k | 2.25 | 9.08 | 4.04x |
| `ch11_m11_9` | 1.44x | 5.08k | 0.00758 | 1.36 | 179x |
| `covid19imperial_v3` | 1.43x | 473k | 119 | 175 | 1.47x |
| `covid19imperial_v2` | 1.43x | 473k | 119 | 175 | 1.47x |
| `aalto_binom` | 1.42x | 5.11k | 0.00652 | 1.14 | 175x |
| `aalto_bern` | 1.42x | 5.11k | 0.00672 | 1.29 | 191x |
| `sw_cumulative_cs` | 1.41x | 22.3k | 1.45 | 5.75 | 3.98x |
| `s2_zoi_beta` | 1.4x | 12.1k | 0.062 | 2.62 | 42.2x |
| `s2_zi_asymlaplace` | 1.4x | 28.3k | 0.0971 | 2.59 | 26.7x |
| `sw_exgaussian` | 1.4x | 25.1k | 0.0325 | 2.67 | 82.2x |
| `s2_cumulative_probit` | 1.39x | 13.7k | 0.0665 | 2.52 | 37.9x |
| `s2_zi_beta` | 1.38x | 10.2k | 0.0533 | 2.52 | 47.3x |
| `Rate_5_model` | 1.38x | 5.33k | 0.00744 | 1.25 | 169x |
| `Rate_2_model` | 1.38x | 7.31k | 0.00738 | 1.28 | 173x |
| `ch15_m15_7` | 1.38x | 28k | 0.264 | 6.7 | 25.4x |
| `Rate_1_model` | 1.37x | 4.69k | 0.00585 | 1.13 | 194x |
| `irt_2pl` | 1.37x | 61.4k | 1.1 | 4.46 | 4.06x |
| `s2_mixture_theta` | 1.36x | 80.4k | 0.488 | 3.84 | 7.88x |
| `Rate_3_model` | 1.36x | 5.2k | 0.00726 | 1.15 | 158x |
| `aalto_binom2` | 1.36x | 7.7k | 0.00766 | 1.25 | 163x |
| `i320_gp_expquad` | 1.36x | 52.4k | 0.572 | 6.84 | 12x |
| `wells_dist` | 1.35x | 26.5k | 0.657 | 2.61 | 3.96x |
| `i319_negbin_re` | 1.35x | 66.7k | 0.656 | 4.44 | 6.76x |
| `GLM_Binomial_model` | 1.33x | 13.3k | 0.019 | 2.04 | 108x |
| `ch14_m14_8nc` | 1.33x | 263k | 0.734 | 5.71 | 7.79x |
| `aalto_binomb` | 1.33x | 5.61k | 0.00699 | 1.16 | 166x |
| `ch12_m12_2` | 1.32x | 64.2k | 0.064 | 2.82 | 44x |
| `ch12_m12_1` | 1.32x | 11.8k | 0.0196 | 1.97 | 101x |
| `ch11_m_pois` | 1.32x | 6.85k | 0.00732 | 1.26 | 172x |
| `ch12_m12_6` | 1.31x | 111k | 222 | 294 | 1.32x |
| `blr` | 1.3x | 32.4k | 0.0186 | 1.84 | 98.5x |
| `sw_gp` | 1.26x | 47.3k | 0.92 | 7.24 | 7.87x |
| `sw_beta` | 1.25x | 8.99k | 0.0251 | 2.68 | 107x |
| `ch14_m14_6x` | 1.25x | 21.6k | 1.89 | 7.9 | 4.19x |
| `ch14_m14_6` | 1.25x | 21.6k | 1.87 | 7.88 | 4.2x |
| `ch14_m14_9` | 1.24x | 12.5k | 4.37 | 8.43 | 1.93x |
| `ch14_m14_1` | 1.24x | 37.6k | 0.205 | 5.96 | 29.1x |
| `ch14_m14_10` | 1.23x | 11.2k | 4.15 | 8.13 | 1.96x |
| `sw_mixture` | 1.23x | 303k | 1.7 | 5.56 | 3.26x |
| `nn_rbm1bJ10` | 1.23x | 2.03M | 330 | 408 | 1.24x |
| `sw_mv_rescor` | 1.22x | 13.3k | 0.0896 | 5.03 | 56.2x |
| `ch14_m14_8` | 1.21x | 204k | 0.577 | 4.25 | 7.36x |
| `s2_frechet` | 1.2x | 33.2k | 0.073 | 2.8 | 38.4x |
| `sw_bernoulli` | 1.18x | 8.63k | 0.0117 | 2.05 | 175x |
| `sw_categorical` | 1.18x | 11k | 0.0219 | 3.17 | 145x |
| `logistic_regression_rhs` | 1.18x | 137k | 6.02 | 10.9 | 1.8x |
| `s2_custom_vint` | 1.17x | 10.5k | 0.0599 | 2.36 | 39.4x |
| `ch14_m14_7` | 1.16x | 143k | 4.11 | 12.4 | 3.01x |
| `sw_binomial` | 1.15x | 7.52k | 0.0224 | 2.28 | 102x |
| `sw_weibull` | 1.15x | 10.3k | 0.0287 | 2.7 | 94.2x |
| `Survey_model` | 1.14x | 11.1k | 0.65 | 2.44 | 3.75x |
| `i320_sratio_plain` | 1.13x | 15.4k | 0.924 | 3.86 | 4.17x |
| `sw_poisson` | 1.13x | 9.85k | 0.0131 | 1.89 | 144x |
| `s2_multinomial` | 1.12x | 12.9k | 0.129 | 2.87 | 22.3x |
| `sw_cratio` | 1.12x | 10.4k | 0.631 | 3.36 | 5.33x |
| `sw_sratio` | 1.09x | 10.4k | 0.631 | 3.35 | 5.31x |
| `s2_discrete_weibull` | 1.08x | 31.8k | 0.12 | 2.39 | 19.9x |
| `s2_beta_binomial` | 1.08x | 10.4k | 0.0521 | 2.53 | 48.4x |
| `sw_mono` | 1.08x | 20.4k | 0.044 | 2.92 | 66.4x |
| `nn_rbm1bJ100` † | 1.07x | 5k | 2.19e+03 | 2.36e+03 | 1.07x |
| `s2_weights_trunc` | 1.07x | 25.1k | 0.173 | 2.54 | 14.7x |
| `ch14_m14_11` | 1.05x | 12.7k | 8.01 | 11.6 | 1.45x |
| `sw_negbinomial` | 1.05x | 1.49M | 2.09 | 4.36 | 2.09x |
| `s2_dirichlet` | 1.05x | 12.6k | 0.169 | 3.3 | 19.5x |
| `sw_trunc` | 1.05x | 27.2k | 0.181 | 2.48 | 13.8x |
| `wells_dist100ars_model` | 1.04x | 19.1k | 0.366 | 2.37 | 6.47x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0738 | 1.94 | 26.3x |
| `hmm_gaussian` | 1.04x | 63.9k | 17.8 | 21.9 | 1.23x |
| `nes_logit_model` | 1.03x | 17.8k | 0.126 | 2.05 | 16.2x |
| `wells_dae_model` | 1.03x | 24.2k | 0.502 | 2.5 | 4.97x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.327 | 2.47 | 7.56x |
| `wells_dist100_model` | 1.02x | 11.5k | 0.204 | 2.19 | 10.7x |
| `s2_mo_simo_prior` | 1.02x | 21.3k | 0.0476 | 2.92 | 61.4x |
| `wells_daae_c_model` | 1.02x | 17.6k | 0.382 | 2.54 | 6.65x |
| `hmm_drive_1` | 1.02x | 39.6k | 6.1 | 9.71 | 1.59x |
| `bones_model` | 1.02x | 14.7k | 0.782 | 3.02 | 3.86x |
| `wells_dae_inter_model` | 1.02x | 12.8k | 0.284 | 2.42 | 8.53x |
| `s2_hurdle_negbin` | 1.02x | 15.2k | 0.114 | 2.37 | 20.8x |
| `i319_negbin_fixed` | 1.02x | 19.4k | 0.186 | 2.35 | 12.6x |
| `i320_pois_trunc_ub` | 1.02x | 21.9k | 0.572 | 2.79 | 4.88x |
| `diamonds` | 1.02x | 1.51M | 49.2 | 52.2 | 1.06x |
| `wells_interaction_model` | 1.01x | 32.1k | 0.672 | 2.68 | 3.99x |
| `s2_wiener` | 1.01x | 14.7k | 0.368 | 2.79 | 7.58x |
| `aalto_poisson_simple` | 1.01x | 5.02k | 0.0124 | 1.38 | 112x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 11.2k | 0.236 | 2.33 | 9.88x |
| `cm_betadiscrete` | 1x | 9.91k | 23 | 26.2 | 1.14x |
| `sw_cumulative` | 0.994x | 9.89k | 0.228 | 2.74 | 12x |
| `one_comp_mm_elim_abs` | 0.984x | 18.9k | 8.86 | 11.3 | 1.27x |
| `iohmm_reg` | 0.983x | 462k | 157 | 159 | 1.01x |
| `i320_pois_trunc_both` | 0.98x | 21.9k | 0.727 | 2.97 | 4.09x |
| `cm_ddm` † | 0.974x | 5k | 841 | 827 | 0.983x |
| `s2_ar_cov` | 0.968x | 11.2k | 0.0868 | 6.44 | 74.2x |
| `s2_cosy` | 0.966x | 14.7k | 0.104 | 5.65 | 54.5x |
| `s2_fcor` | 0.961x | 9.87k | 0.146 | 3.8 | 26.1x |
| `s2_mi_trunc_lb` | 0.959x | 37.9k | 0.217 | 3.39 | 15.6x |
| `hmm_drive_0` | 0.951x | 24.5k | 3.72 | 7.08 | 1.9x |
| `i320_gp_matern32` | 0.939x | 133k | 4.13 | 9.91 | 2.4x |
| `cm_invgaussian` † | 0.836x | 5k | 0.873 | 32.6 | 37.3x |
| `garch11` | 0.821x | 23.3k | 0.206 | 1.85 | 8.98x |
| `aalto_gpareto` | 0.73x | 9.37k | 0.016 | 1.84 | 115x |
| `arma11` | 0.694x | 11.2k | 0.0711 | 1.76 | 24.7x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling | Reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `kronecker_gp` | — | — | — | — | — | failed; point 0: density/gradient mismatch: scaled error 0.0723 |
<!--/gen-->

## Fast mode

<!--gen:fast_lead-->
These figures are from 2026-10-21 (Stanli source `051bb4ba`). [Fast
mode](fast-mode.md) used the same protocol, inputs and binaries as the default
run, with models compiled and run in fast mode (`corpus_bench.py
--fast-math`).
<!--/gen-->
Gradient times, <!--gen:fast_population-->342<!--/gen--> models with paired timings, Apple M3 Ultra:

- Fast mode is <!--gen:fast_geomean-->1.92x<!--/gen--> faster than default mode in geometric mean; the median
  model is <!--gen:fast_median-->1.03x<!--/gen--> faster. <!--gen:fast_ge_two-->81<!--/gen--> models are at least 2x faster, <!--gen:fast_ge_ten-->42<!--/gen--> at least 10x
  and <!--gen:fast_ge_hundred-->8<!--/gen--> at least 100x. Most of the large gains are models whose likelihood
  repeats over the data, which fast mode evaluates once per distinct row or
  group.
- Against CmdStan, the geometric-mean speedup is <!--gen:fast_cmdstan_default-->2.24x<!--/gen--> 
  in default mode and <!--gen:fast_cmdstan_fast-->4.28x<!--/gen--> in fast mode. Fast mode beats CmdStan on <!--gen:fast_beats_fast-->334<!--/gen--> of the <!--gen:fast_population-->342<!--/gen--> models,
  default mode on <!--gen:fast_beats_default-->328<!--/gen-->.
- In fast mode, <!--gen:fast_slowest-->the slowest model, `s2_invgaussian`, is 1.06x slower.<!--/gen-->

The largest gains, gradient time in microseconds:

<!--gen:fast_top_table-->
| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 41.7 | 0.0817 | 511x faster |
| `nes` | 18.5 | 0.0713 | 259x faster |
| `logearn_interaction` | 7.93 | 0.0519 | 153x faster |
| `logearn_interaction_z` | 7.94 | 0.0523 | 152x faster |
| `logearn_height_male` | 5.67 | 0.0462 | 123x faster |
| `logearn_logheight_male` | 5.6 | 0.0461 | 121x faster |
| `ch12_m12_4` | 68.4 | 0.643 | 106x faster |
| `diamonds` | 32.6 | 0.315 | 104x faster |
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

Only `kronecker_gp` fails the gate, in all three runs, with a scaled error of
`0.072`. No model drops a point.
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
| Measured source | <!--gen:benchmark_source-->`051bb4ba6834a7d6f313bdbabc2adc861bcf7b1d`<!--/gen-->; tracked differences and executable hashes in manifests |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan | <!--gen:benchmark_cmdstan-->2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`<!--/gen--> |
| Stan Math | <!--gen:benchmark_stan_math-->`5252d51d47c1d5e78005fc043ad996fad6dd8da8`<!--/gen--> |
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
