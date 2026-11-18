# Benchmarks

## TL;DR

- Stanli's gradients are **<!--gen:corpus_median-->2.17x<!--/gen--> faster** than CmdStan's for the median
  of <!--gen:benchmark_models-->347<!--/gen--> application models, and [total sampling](#full-corpus) is **<!--gen:corpus_total_median-->25x<!--/gen--> faster**,
  mostly because CmdStan builds C++ for each model. Stanli binds
  data-dependent structure once and reuses its plan and buffers
  ([how it works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)).
  Measured on <!--gen:benchmark_hardware-->Apple M3 Ultra<!--/gen-->.
- Stanli is as fast or faster on <!--gen:corpus_at_par-->335<!--/gen--> of the <!--gen:corpus_n_grad-->346<!--/gen--> models that pass the
  numerical check. Regressions, GLMs and IRT models gain the most, because
  independent observations batch into vector operations
  ([loop recovery](how-it-works.md#recovering-vector-operations-from-scalar-loops),
  [optimization details](../runtime/src/OPTIMIZATIONS.md)). Dense linear algebra, ODE
  solvers and sequential recurrences spend their time in Stan Math in both
  engines and land near parity or slower.
- [Fast mode](#fast-mode) is <!--gen:fast_cmdstan_fast-->4.27x<!--/gen--> faster than CmdStan in geometric mean.

<a id="full-corpus"></a>

## Full corpus results

Every application model uses the same settings. Language-conformance fixtures
belong to the [numerical corpus](corpus-status.md) and are not timed here.
CmdStan uses its default compiler settings. The `CmdStan / Stanli gradient`
column is how many times faster Stanli's gradient is, and rows are sorted by it.

Total sampling is each engine's setup time plus the gradients a 1,000 warmup +
1,000 draw run takes, with the gradient count from a reference run
([method](#total-sampling-estimate)). The last column is how many times faster
Stanli's total sampling is.

<!--gen:benchmark_table-->
| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 34.2x | 18.6k | 1.26 | 44.3 | 35.3x |
| `gpcm_latent_reg_irt` | 12.5x | 69k | 8.16 | 105 | 12.9x |
| `grsm_latent_reg_irt` | 11.9x | 63.1k | 4.45 | 56.9 | 12.8x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.3x | 47.3k | 1.27 | 15.6 | 12.3x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11.1x | 68.5k | 1.84 | 21.9 | 11.9x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10.1x | 81.2k | 3.76 | 39.2 | 10.4x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 50.6k | 2.31 | 24.6 | 10.7x |
| `radon_county_intercept` | 10x | 62.8k | 2.86 | 29.9 | 10.4x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.78x | 52.9k | 2.48 | 25.7 | 10.4x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.6x | 102k | 4.85 | 48 | 9.91x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.42x | 77.7k | 5.11 | 49.5 | 9.69x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.32x | 100k | 6.51 | 62.3 | 9.57x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.23x | 59.7k | 3.09 | 30 | 9.7x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.06x | 125k | 6.58 | 61.4 | 9.33x |
| `aalto_poisson_hurdle` | 8.91x | 6.92k | 0.0608 | 2.31 | 37.9x |
| `radon_pooled` | 8.52x | 9.33k | 0.398 | 4.74 | 11.9x |
| `s2_custom_vreal` | 8.04x | 10.2k | 0.0128 | 2.34 | 182x |
| `dogs` | 7.76x | 20.7k | 0.2 | 4.06 | 20.3x |
| `rats_model` | 7.59x | 54.5k | 0.0474 | 2.14 | 45.1x |
| `arK` | 7.27x | 59.1k | 0.108 | 2.53 | 23.4x |
| `ch12_m12_3_alt` | 7.2x | 11k | 0.0354 | 1.58 | 44.6x |
| `ch12_m12_3` | 7.1x | 10.8k | 0.0373 | 1.6 | 42.8x |
| `ch16_m16_1` | 6.88x | 301k | 1.21 | 10.3 | 8.48x |
| `M0_model` | 6.4x | 7.46k | 0.0281 | 1.71 | 60.7x |
| `Mt_model` | 5.55x | 11k | 0.05 | 2.67 | 53.4x |
| `sw_nonlinear` | 5.35x | 10.9k | 0.0138 | 2.74 | 198x |
| `ctsem_ctsm_400` † | 5.14x | 5k | 19.8 | 198 | 9.99x |
| `dogs_log` | 5.13x | 12.1k | 0.12 | 2.73 | 22.8x |
| `ctsem_ctsm` | 5.11x | 1.08M | 432 | 2.29e+03 | 5.29x |
| `ch09_m5_8s` | 4.82x | 1.37M | 0.627 | 4.76 | 7.6x |
| `ch09_m5_8s2` | 4.76x | 1.11M | 0.521 | 4.23 | 8.13x |
| `s2_invgaussian` | 4.76x | 16.3k | 0.0149 | 2.55 | 171x |
| `ch14_m14_5` | 4.56x | 12k | 0.0355 | 1.88 | 52.9x |
| `s2_cumulative_cauchit` | 4.45x | 20.7k | 0.028 | 2.56 | 91.4x |
| `logmesquite_logvas` | 4.26x | 92.3k | 0.0505 | 2.35 | 46.5x |
| `mesquite` | 4.25x | 160k | 0.0794 | 2.17 | 27.3x |
| `ch15_m15_1` | 4.25x | 30.7k | 0.0197 | 2.21 | 113x |
| `logmesquite_logvash` | 4.17x | 93.7k | 0.0461 | 2.29 | 49.7x |
| `logmesquite` | 4.17x | 56.4k | 0.0346 | 2.02 | 58.3x |
| `s2_nl_noloop` | 4.16x | 52.6k | 0.0342 | 2.87 | 83.9x |
| `ch15_m15_2` | 4.13x | 32.4k | 0.027 | 2.48 | 91.9x |
| `logmesquite_logva` | 4.13x | 58.7k | 0.023 | 2.02 | 88x |
| `s2_car_icar` | 4.07x | 1.7M | 0.715 | 6.14 | 8.59x |
| `s2_logistic_normal` | 4.06x | 13.6k | 0.0834 | 5.57 | 66.8x |
| `nes` | 3.97x | 90.8k | 1.68 | 8.63 | 5.13x |
| `election88_full` | 3.96x | 429k | 102 | 410 | 4x |
| `ch14_m14_4x` | 3.88x | 9.05k | 0.0215 | 1.76 | 82.2x |
| `sw_me` | 3.86x | 57.9k | 0.0415 | 3.07 | 73.8x |
| `ch14_m14_4` | 3.85x | 9.05k | 0.0218 | 1.76 | 81x |
| `logmesquite_logvolume` | 3.79x | 12.4k | 0.00937 | 1.8 | 192x |
| `sw_dist_sigma` | 3.76x | 10.7k | 0.0166 | 2.68 | 162x |
| `sw_spline_s` | 3.76x | 37.6k | 0.0279 | 3.36 | 120x |
| `Mth_model` | 3.73x | 33.9k | 0.911 | 6.77 | 7.43x |
| `s2_s_cc` | 3.7x | 30.5k | 0.0216 | 3.19 | 148x |
| `aalto_grp_aov` | 3.67x | 12.5k | 0.00861 | 2.03 | 236x |
| `s2_dist_sigma_re` | 3.65x | 29.4k | 0.0299 | 3.83 | 128x |
| `ch09_m9_1` | 3.63x | 11.3k | 0.0147 | 2.04 | 139x |
| `ch09_m9_1_chains4` | 3.61x | 11.3k | 0.0148 | 2.04 | 138x |
| `sw_weights` | 3.6x | 9.98k | 0.0149 | 2.33 | 156x |
| `kidscore_interaction_c2` | 3.6x | 17.7k | 0.0537 | 1.95 | 36.3x |
| `s2_me2_nomecor` | 3.57x | 50.4k | 0.0567 | 3.41 | 60.1x |
| `s2_nlf` | 3.56x | 9.42k | 0.0166 | 2.82 | 170x |
| `kilpisjarvi` | 3.56x | 1.12M | 0.228 | 2.34 | 10.3x |
| `s2_mm` | 3.54x | 26.6k | 0.0283 | 3.57 | 126x |
| `sw_se` | 3.52x | 7.2k | 0.0115 | 2.33 | 203x |
| `kidscore_interaction_z` | 3.52x | 24k | 0.0704 | 2.08 | 29.6x |
| `kidscore_interaction_c` | 3.51x | 15.2k | 0.0467 | 1.94 | 41.5x |
| `kidscore_interaction` | 3.49x | 178k | 0.476 | 3.42 | 7.19x |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.48x | 17.3k | 0.00857 | 2.27 | 265x |
| `s2_mm_weights` | 3.46x | 27.8k | 0.0296 | 3.57 | 120x |
| `kidscore_mom_work` | 3.46x | 40.5k | 0.115 | 2.07 | 18.1x |
| `s2_shifted_lognormal` | 3.44x | 15.1k | 0.0157 | 2.79 | 178x |
| `logearn_interaction` | 3.44x | 322k | 2.57 | 10.5 | 4.09x |
| `s2_cox_cens` | 3.43x | 22.1k | 0.0323 | 4.03 | 125x |
| `logearn_height_male` | 3.41x | 189k | 1.07 | 5.3 | 4.94x |
| `s2_gp_approx` | 3.4x | 34k | 0.032 | 4.15 | 129x |
| `ch15_m15_9` | 3.37x | 11.3k | 0.0329 | 2.01 | 61.3x |
| `ch15_m15_5` | 3.36x | 21.5k | 0.0171 | 2.23 | 130x |
| `logearn_logheight_male` | 3.34x | 715k | 4.08 | 15.3 | 3.75x |
| `logearn_interaction_z` | 3.34x | 22k | 0.182 | 2.5 | 13.7x |
| `sw_lognormal` | 3.33x | 8.28k | 0.0111 | 2.55 | 229x |
| `s2_s_by` | 3.33x | 70.7k | 0.117 | 5.08 | 43.3x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.32x | 1.03M | 6.43 | 25 | 3.88x |
| `GLMM1_model` | 3.32x | 32.8k | 0.33 | 3.31 | 10x |
| `s2_car_esicar` | 3.31x | 28.9k | 0.0278 | 3.4 | 122x |
| `ch16_m16_4` | 3.31x | 35.3k | 0.0636 | 2.03 | 31.9x |
| `s2_car` | 3.28x | 30.6k | 0.0304 | 3.27 | 108x |
| `sw_spline_t2` | 3.28x | 38.1k | 0.0478 | 3.89 | 81.2x |
| `kidscore_momhsiq` | 3.26x | 90.5k | 0.186 | 2.32 | 12.5x |
| `radon_county` | 3.23x | 48.8k | 1.28 | 5.96 | 4.68x |
| `s2_t2_by` | 3.21x | 36.4k | 0.0714 | 4.9 | 68.6x |
| `s2_gp_by_approx` | 3.2x | 37.2k | 0.055 | 5.01 | 91.2x |
| `ch15_m15_8` | 3.19x | 10.7k | 0.033 | 1.84 | 55.9x |
| `s2_gr_by` | 3.15x | 33.2k | 0.0277 | 3.85 | 139x |
| `cm_exwald` | 3.11x | 68k | 3.02 | 12.5 | 4.16x |
| `ch15_m15_6` | 3.09x | 15.6k | 0.0111 | 1.92 | 173x |
| `aalto_grp_prior_mean` | 3.08x | 19.3k | 0.00929 | 2.14 | 231x |
| `i319_gauss_re` | 3.06x | 41.3k | 0.0725 | 3.94 | 54.4x |
| `s2_cox` | 3.04x | 30.6k | 0.0339 | 3.08 | 90.7x |
| `sw_re_gauss` | 3.02x | 29.1k | 0.0248 | 3.46 | 140x |
| `aalto_lin` | 2.96x | 18.2k | 0.0105 | 2.04 | 195x |
| `logearn_height` | 2.91x | 138k | 0.525 | 3.13 | 5.95x |
| `eight_schools_centered` | 2.9x | 32.4k | 0.00995 | 1.92 | 193x |
| `sw_acat` | 2.9x | 12.2k | 0.619 | 4.48 | 7.23x |
| `log10earn_height` | 2.89x | 143k | 0.536 | 3.12 | 5.83x |
| `sw_student` | 2.88x | 12.7k | 0.0158 | 2.71 | 171x |
| `earn_height` | 2.88x | 150k | 0.565 | 3.33 | 5.88x |
| `s2_hurdle_cumulative` | 2.87x | 11.2k | 0.0392 | 3.06 | 78x |
| `pilots` | 2.87x | 551k | 0.187 | 2.68 | 14.3x |
| `s2_mi_lognormal` | 2.87x | 14.8k | 0.0215 | 3.47 | 162x |
| `s2_cumulative_cloglog` | 2.85x | 20.3k | 0.033 | 2.53 | 76.6x |
| `bym2_offset_only` | 2.85x | 202k | 7.64 | 25.1 | 3.29x |
| `GLMM_Poisson_model` | 2.85x | 75.6k | 0.0529 | 3.02 | 57.1x |
| `kidscore_momiq` | 2.84x | 60.1k | 0.0887 | 1.87 | 21.1x |
| `aalto_lin_std` | 2.84x | 11k | 0.00892 | 2.28 | 255x |
| `aalto_grp_prior_mean_var` | 2.81x | 59.3k | 0.023 | 3.03 | 132x |
| `Mtbh_model` | 2.81x | 34.3k | 0.572 | 5.79 | 10.1x |
| `s2_gr_student` | 2.79x | 52.2k | 0.0455 | 4.14 | 90.9x |
| `blr` | 2.79x | 32.4k | 0.0187 | 2.14 | 114x |
| `sw_hurdle_lognormal` | 2.79x | 12.5k | 0.0228 | 2.42 | 106x |
| `GLM_Poisson_model` | 2.77x | 29.5k | 0.0188 | 2.31 | 123x |
| `dogs_nonhierarchical` | 2.76x | 56.4k | 0.893 | 8.84 | 9.9x |
| `sesame_one_pred_a` | 2.75x | 15.5k | 0.0188 | 1.62 | 85.8x |
| `s2_mmc` | 2.73x | 35.4k | 0.0619 | 6.06 | 98x |
| `kidscore_momhs` | 2.71x | 24.8k | 0.041 | 1.73 | 42.3x |
| `sw_gamma` | 2.7x | 9.45k | 0.0152 | 2.6 | 171x |
| `accel_gp` | 2.67x | 1.5M | 5.2 | 18.6 | 3.58x |
| `ch13_m13_4nc` | 2.64x | 75.5k | 0.673 | 4.05 | 6.02x |
| `cm_geg` | 2.61x | 54k | 1.38 | 6.49 | 4.71x |
| `hierarchical_gp` | 2.6x | 352k | 6.01 | 24.4 | 4.05x |
| `ch12_m12_7` | 2.6x | 48.6k | 48.9 | 129 | 2.64x |
| `sw_hurdle_pois` | 2.6x | 10.4k | 0.0273 | 2.22 | 81x |
| `sw_acat_cs` | 2.58x | 18.3k | 1.24 | 7.05 | 5.68x |
| `ch12_m12_5` | 2.58x | 37.4k | 38.8 | 102 | 2.64x |
| `losscurve_sislob` | 2.57x | 21.3k | 0.0322 | 3.18 | 98.9x |
| `s2_gev` | 2.56x | 12.2k | 0.0344 | 2.68 | 77.8x |
| `cm_exgaussian` | 2.55x | 14k | 0.223 | 3.26 | 14.6x |
| `s2_sar` | 2.53x | 10.9k | 0.0499 | 3 | 60.2x |
| `accel_splines` | 2.53x | 1.86M | 7.1 | 21.4 | 3.02x |
| `ch11_m11_10` | 2.52x | 10.7k | 0.0093 | 2.24 | 241x |
| `s2_me2` | 2.48x | 71.6k | 0.132 | 5.99 | 45.5x |
| `s2_sar_error` | 2.47x | 11.4k | 0.0517 | 3.06 | 59.2x |
| `ldaK5` † | 2.46x | 5k | 12.7 | 32.6 | 2.57x |
| `s2_rate` | 2.44x | 7.8k | 0.0122 | 2.46 | 202x |
| `ch14_m14_3` | 2.43x | 65.2k | 0.595 | 6.66 | 11.2x |
| `seeds_centered_model` | 2.42x | 41.5k | 0.0359 | 2.75 | 76.7x |
| `logistic_regression_rhs` | 2.37x | 137k | 6.07 | 18.2 | 2.99x |
| `i319_pois_re2` | 2.36x | 173k | 0.839 | 6 | 7.16x |
| `ch15_m15_3` | 2.35x | 7.74k | 0.108 | 1.75 | 16.2x |
| `Mh_model` | 2.33x | 53.8k | 0.908 | 4.39 | 4.84x |
| `dogs_hierarchical` | 2.33x | 9.37k | 0.169 | 2.03 | 12x |
| `sw_re_bern` | 2.32x | 27.4k | 0.0281 | 3.54 | 126x |
| `ch09_m9_5` | 2.32x | 398k | 0.0591 | 1.57 | 26.5x |
| `ch09_m9_4` | 2.31x | 1.55M | 0.215 | 1.92 | 8.95x |
| `s2_mv_shared_re` | 2.3x | 36.8k | 0.066 | 6.17 | 93.5x |
| `oncobayes2_blrm_exnex_sa` | 2.29x | 25.3k | 0.218 | 12.5 | 57.3x |
| `extra_hurdle_poisson` | 2.29x | 8.4k | 0.00832 | 1.47 | 177x |
| `ch09_m9_3` | 2.29x | 10.8k | 0.00736 | 1.41 | 191x |
| `normal_mixture` | 2.28x | 7.96k | 0.305 | 2.02 | 6.61x |
| `dugongs_model` | 2.28x | 33.4k | 0.0219 | 2.01 | 91.8x |
| `ch15_m15_4` | 2.27x | 8.81k | 0.103 | 1.74 | 17x |
| `aalto_lin_std_t` | 2.27x | 13k | 0.0112 | 2.52 | 226x |
| `i320_mi_nhanes` | 2.26x | 312k | 0.277 | 4.16 | 15x |
| `sw_skewnormal` | 2.24x | 15.5k | 0.0253 | 2.85 | 112x |
| `ch09_m9_2` | 2.24x | 46.7k | 0.00967 | 1.41 | 146x |
| `ch11_m11_5` | 2.23x | 29.3k | 0.202 | 2.48 | 12.3x |
| `sw_re_slope` | 2.22x | 53.4k | 0.0682 | 5.88 | 86.2x |
| `sw_vonmises` | 2.22x | 730k | 0.514 | 3.64 | 7.08x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.19x | 44.4k | 2.11 | 6.5 | 3.07x |
| `ch13_m13_7` | 2.19x | 19.9k | 0.00686 | 1.18 | 171x |
| `hier_2pl` | 2.18x | 59.4k | 12.1 | 33 | 2.72x |
| `low_dim_gauss_mix` | 2.18x | 14k | 0.689 | 3.48 | 5.05x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo2</code> | 2.17x | 34.3k | 0.475 | 13 | 27.5x |
| `sw_re_pois` | 2.17x | 30k | 0.0336 | 3.4 | 101x |
| `ch13_m13_4` | 2.16x | 34.4k | 0.245 | 2.92 | 11.9x |
| `ldaK2` | 2.15x | 19.8k | 1.02 | 4.76 | 4.68x |
| `ch13_m13_4b` | 2.15x | 34.4k | 0.246 | 2.8 | 11.3x |
| `lsat_model` | 2.13x | 40.8k | 1.84 | 6.45 | 3.51x |
| `ch13_m13_6` | 2.13x | 36.1k | 0.259 | 2.93 | 11.3x |
| `2pl_latent_reg_irt` | 2.11x | 51.2k | 3.43 | 12.5 | 3.64x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo3</code> | 2.1x | 33.5k | 1.24 | 14.6 | 11.8x |
| `s2_index_mi` | 2.1x | 18.6k | 0.0264 | 3.09 | 117x |
| `ch09_mp` | 2.07x | 38.9k | 0.00812 | 1.16 | 143x |
| `seeds_model` | 2.07x | 38.4k | 0.0328 | 2.49 | 76.1x |
| `sw_zi_poisson` | 2.06x | 11.5k | 0.0229 | 2.2 | 95.8x |
| `multi_occupancy` | 2.06x | 98.9k | 3.02 | 11.1 | 3.67x |
| `cm_bisa` | 2.04x | 27.9k | 0.644 | 4.44 | 6.89x |
| `seeds_stanified_model` | 2.03x | 42.2k | 0.0341 | 2.41 | 70.6x |
| `cm_lognormal` | 2.02x | 68k | 2.64 | 8.57 | 3.24x |
| `irt_2pl` | 2.02x | 61.4k | 1.08 | 5.13 | 4.74x |
| `sw_exgaussian` | 2.01x | 25.1k | 0.0312 | 2.8 | 89.9x |
| `ch11_m11_8` | 2x | 50.9k | 0.0207 | 1.89 | 91.5x |
| `ch13_m13_7nc` | 2x | 7.88k | 0.00668 | 1.21 | 182x |
| `prophet` | 1.99x | 1.66M | 61.7 | 128 | 2.07x |
| `cm_rdm` | 1.99x | 67.7k | 8.39 | 20.1 | 2.39x |
| `cm_lnr` | 1.98x | 41.9k | 4.75 | 12.7 | 2.68x |
| `sw_cens` | 1.98x | 10.8k | 0.0241 | 3.17 | 132x |
| `ch13_m13_5` | 1.98x | 18.9k | 0.122 | 2.45 | 20.1x |
| `ch11_m11_4` | 1.96x | 16.5k | 0.107 | 2.17 | 20.2x |
| `soil_incubation` | 1.96x | 203k | 4.82 | 11.7 | 2.43x |
| `normal_mixture_k` | 1.95x | 243k | 48 | 96.6 | 2.01x |
| `ch11_m11_6` | 1.95x | 16.7k | 0.0151 | 1.98 | 131x |
| `cm_loggamma` | 1.89x | 226k | 6.72 | 16 | 2.38x |
| `ch13_m13_1` | 1.87x | 18.5k | 0.0194 | 1.92 | 99.3x |
| `s2_cens_interval` | 1.87x | 8.94k | 0.0233 | 2.61 | 112x |
| `sw_hurdle_gamma` | 1.86x | 13.3k | 0.0361 | 2.42 | 66.9x |
| `sw_arma` | 1.86x | 42.2k | 0.103 | 3.37 | 32.8x |
| `ch13_m13_2` | 1.85x | 22.9k | 0.0244 | 2.11 | 86.4x |
| `ch13_m13_3` | 1.84x | 27.8k | 0.0304 | 2.02 | 66.3x |
| `lotka_volterra` | 1.83x | 75.1k | 2.52 | 7.73 | 3.07x |
| `sw_mi` | 1.82x | 15.1k | 0.0204 | 2.72 | 134x |
| `sw_ar` | 1.81x | 10.4k | 0.0268 | 3.12 | 117x |
| `i319_pois_re` | 1.79x | 192k | 0.72 | 4.92 | 6.84x |
| `sw_ma` | 1.78x | 11.1k | 0.0281 | 3.14 | 112x |
| `cm_logweibull` | 1.78x | 90.4k | 1.87 | 6.41 | 3.43x |
| `ch11_m11_11` | 1.76x | 143k | 0.0612 | 2.67 | 43.7x |
| `sw_beta` | 1.75x | 8.99k | 0.0249 | 2.74 | 110x |
| `s2_threading` | 1.73x | 11.8k | 0.012 | 2.22 | 185x |
| `cm_logstudent` | 1.73x | 2.02M | 30.5 | 56.1 | 1.84x |
| `cm_choco` | 1.73x | 15.1k | 0.689 | 4.81 | 6.97x |
| `sw_zi_binomial` | 1.72x | 10.2k | 0.0321 | 2.24 | 69.8x |
| `cm_weibull` | 1.7x | 2.03M | 37.4 | 66.4 | 1.77x |
| `wells_dist` | 1.7x | 26.5k | 0.658 | 2.81 | 4.27x |
| `cm_lba1` | 1.69x | 145k | 7.92 | 16.7 | 2.11x |
| `s2_mv_subset` | 1.68x | 12.1k | 0.0165 | 2.3 | 140x |
| `sw_asymlaplace` | 1.65x | 23.4k | 0.0573 | 2.44 | 42.6x |
| `cm_gamma` | 1.64x | 2.01M | 42 | 71.4 | 1.7x |
| `sw_gaussian` | 1.63x | 10.9k | 0.0113 | 2.11 | 187x |
| `cm_betagate` | 1.62x | 11.8k | 0.419 | 3.71 | 8.84x |
| `s2_zoi_beta` | 1.62x | 12.1k | 0.0578 | 2.57 | 44.4x |
| `sw_mv_norescor` | 1.62x | 11.6k | 0.0161 | 2.3 | 143x |
| `surgical_model` | 1.61x | 22.2k | 0.0166 | 2.23 | 134x |
| `s2_zi_beta` | 1.6x | 10.2k | 0.0527 | 2.53 | 48x |
| `sir` | 1.6x | 127k | 20.2 | 35.5 | 1.75x |
| `GLM_Binomial_model` | 1.59x | 13.3k | 0.0194 | 2.3 | 119x |
| `cm_invweibull` | 1.57x | 87.1k | 2.04 | 6.31 | 3.1x |
| `s2_mixture_theta` | 1.56x | 80.4k | 0.454 | 4.07 | 8.96x |
| `cm_lnr_bench` | 1.56x | 40.1k | 10.8 | 20.5 | 1.9x |
| `ch11_m11_7` | 1.55x | 7.03k | 0.00752 | 1.77 | 235x |
| `cm_invgamma` | 1.53x | 79k | 1.82 | 5.86 | 3.22x |
| `Rate_4_model` | 1.53x | 8.3k | 0.00734 | 1.39 | 190x |
| `s2_categorical_re` | 1.53x | 34.6k | 0.184 | 4.63 | 25.1x |
| `s2_frechet` | 1.52x | 33.2k | 0.0728 | 2.9 | 39.8x |
| `gp_pois_regr` | 1.51x | 396k | 0.75 | 6.21 | 8.27x |
| `Mb_model` | 1.5x | 11k | 0.41 | 2.82 | 6.9x |
| `gp_regr` | 1.5x | 8.82k | 0.0271 | 4.79 | 177x |
| `sw_weibull` | 1.5x | 10.3k | 0.0286 | 2.76 | 96.5x |
| `s2_unstr` | 1.5x | 46.4k | 0.228 | 6.77 | 29.7x |
| `sw_re_negbin` | 1.49x | 30.3k | 0.0664 | 3.68 | 55.4x |
| `cm_lba2` | 1.49x | 140k | 13.7 | 24.2 | 1.77x |
| `sw_zi_negbin` | 1.48x | 20.5k | 0.0702 | 2.37 | 33.8x |
| `ch12_m12_6` | 1.46x | 111k | 210 | 313 | 1.49x |
| `covid19imperial_v2` | 1.46x | 473k | 116 | 175 | 1.5x |
| `covid19imperial_v3` | 1.45x | 473k | 119 | 176 | 1.49x |
| `s2_gp_by_gr` | 1.45x | 59.6k | 0.438 | 8.65 | 19.8x |
| `s2_com_poisson` | 1.45x | 42.5k | 0.896 | 3.75 | 4.18x |
| `hmm_example` | 1.45x | 25.1k | 0.495 | 3.86 | 7.8x |
| `aalto_binom` | 1.44x | 5.11k | 0.00638 | 1.25 | 197x |
| `s2_zi_asymlaplace` | 1.42x | 28.3k | 0.101 | 2.55 | 25.2x |
| `aalto_bern` | 1.42x | 5.11k | 0.00705 | 1.44 | 204x |
| `ch14_m14_2` | 1.42x | 135k | 2.25 | 9.81 | 4.37x |
| `Rate_1_model` | 1.42x | 4.69k | 0.00641 | 1.25 | 196x |
| `i320_gp_expquad` | 1.41x | 52.4k | 0.567 | 7.37 | 13x |
| `aalto_binom2` | 1.4x | 7.7k | 0.00694 | 1.39 | 200x |
| `ch15_m15_7` | 1.39x | 28k | 0.263 | 7.07 | 26.9x |
| `ch11_m11_9` | 1.38x | 5.08k | 0.00663 | 1.49 | 224x |
| `Rate_2_model` | 1.38x | 7.31k | 0.00788 | 1.42 | 180x |
| `ch11_m_pois` | 1.36x | 6.85k | 0.00713 | 1.4 | 196x |
| `i319_negbin_re` | 1.35x | 66.7k | 0.657 | 4.81 | 7.32x |
| `sw_mono` | 1.35x | 20.4k | 0.0415 | 2.85 | 68.8x |
| `sw_binomial` | 1.34x | 7.52k | 0.0214 | 2.42 | 113x |
| `Rate_5_model` | 1.34x | 5.33k | 0.00688 | 1.38 | 201x |
| `ch12_m12_2` | 1.34x | 64.2k | 0.0634 | 3.14 | 49.5x |
| `ch14_m14_8nc` | 1.33x | 263k | 0.737 | 5.98 | 8.11x |
| `kronecker_gp` | 1.33x | 2M | 348 | 471 | 1.35x |
| `aalto_binomb` | 1.32x | 5.61k | 0.00609 | 1.28 | 210x |
| `Rate_3_model` | 1.32x | 5.2k | 0.00679 | 1.28 | 189x |
| `ch14_m14_1` | 1.3x | 37.6k | 0.203 | 6.71 | 33.1x |
| `s2_beta_binomial` | 1.29x | 10.4k | 0.0519 | 2.6 | 50.1x |
| `sw_mixture` | 1.29x | 303k | 1.59 | 5.7 | 3.59x |
| `sw_sratio` | 1.29x | 10.4k | 0.628 | 3.22 | 5.13x |
| `ch12_m12_1` | 1.29x | 11.8k | 0.0202 | 2.21 | 109x |
| `s2_mo_simo_prior` | 1.28x | 21.3k | 0.0445 | 2.88 | 64.8x |
| `sw_gp` | 1.27x | 47.3k | 0.916 | 7.15 | 7.81x |
| `ch14_m14_9` | 1.27x | 12.5k | 4.3 | 8.63 | 2.01x |
| `ch14_m14_6` | 1.25x | 21.6k | 1.89 | 8.51 | 4.49x |
| `i320_sratio_cs` | 1.25x | 20.4k | 1.67 | 5.97 | 3.57x |
| `ch14_m14_6x` | 1.25x | 21.6k | 1.88 | 8.48 | 4.5x |
| `nn_rbm1bJ10` | 1.25x | 2.03M | 321 | 406 | 1.27x |
| `sw_mv_rescor` | 1.23x | 13.3k | 0.0891 | 5.25 | 58.9x |
| `sw_cratio_cs` | 1.23x | 13.7k | 1.16 | 5.01 | 4.34x |
| `sw_cratio` | 1.23x | 10.4k | 0.622 | 3.17 | 5.1x |
| `i320_sratio_plain` | 1.22x | 15.4k | 0.93 | 3.71 | 3.99x |
| `s2_custom_vint` | 1.22x | 10.5k | 0.0597 | 2.41 | 40.4x |
| `s2_cumulative_probit` | 1.21x | 13.7k | 0.0657 | 2.63 | 40x |
| `sw_bernoulli` | 1.2x | 8.63k | 0.0123 | 2.07 | 169x |
| `ch14_m14_8` | 1.2x | 204k | 0.585 | 4.38 | 7.48x |
| `ch14_m14_10` | 1.19x | 11.2k | 4.26 | 8.28 | 1.94x |
| `sw_poisson` | 1.18x | 9.85k | 0.0138 | 1.91 | 139x |
| `ch14_m14_7` | 1.15x | 143k | 4.13 | 13.3 | 3.22x |
| `s2_multinomial` | 1.14x | 12.9k | 0.128 | 3.03 | 23.6x |
| `sw_categorical` | 1.14x | 11k | 0.0223 | 3.17 | 142x |
| `s2_discrete_weibull` | 1.14x | 31.8k | 0.118 | 2.41 | 20.4x |
| `Survey_model` | 1.13x | 11.1k | 0.642 | 2.64 | 4.11x |
| `hmm_gaussian` | 1.1x | 63.9k | 16.8 | 22.1 | 1.32x |
| `ch14_m14_11` | 1.09x | 12.7k | 8.05 | 11.8 | 1.46x |
| `sw_negbinomial` | 1.09x | 1.49M | 2.06 | 4.42 | 2.15x |
| `arma11` | 1.08x | 11.2k | 0.0663 | 1.96 | 29.7x |
| `nn_rbm1bJ100` † | 1.08x | 5k | 2.19e+03 | 2.35e+03 | 1.07x |
| `sw_trunc` | 1.07x | 27.2k | 0.177 | 2.59 | 14.7x |
| `s2_hurdle_negbin` | 1.07x | 15.2k | 0.111 | 2.39 | 21.4x |
| `s2_weights_trunc` | 1.07x | 25.1k | 0.179 | 2.66 | 14.8x |
| `hmm_drive_1` | 1.06x | 39.6k | 5.81 | 10 | 1.73x |
| `bones_model` | 1.05x | 14.7k | 0.769 | 3.27 | 4.26x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0729 | 2.16 | 29.6x |
| `nes_logit_model` | 1.03x | 17.8k | 0.126 | 2.06 | 16.3x |
| `iohmm_reg` | 1.03x | 462k | 154 | 163 | 1.06x |
| `s2_dirichlet` | 1.03x | 12.6k | 0.171 | 3.38 | 19.7x |
| `aalto_poisson_simple` | 1.03x | 5.02k | 0.0129 | 1.56 | 121x |
| `wells_dist100_model` | 1.02x | 11.5k | 0.207 | 2.19 | 10.6x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.02x | 11.2k | 0.237 | 2.37 | 10x |
| `i319_negbin_fixed` | 1.02x | 19.4k | 0.185 | 2.59 | 14x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.326 | 2.51 | 7.69x |
| `garch11` | 1.02x | 23.3k | 0.201 | 2.08 | 10.4x |
| `wells_interaction_model` | 1.02x | 32.1k | 0.669 | 2.7 | 4.04x |
| `wells_dae_inter_model` | 1.02x | 12.8k | 0.283 | 2.45 | 8.64x |
| `wells_dae_model` | 1.02x | 24.2k | 0.509 | 2.55 | 5.01x |
| `diamonds` | 1.01x | 1.51M | 49.3 | 52.1 | 1.06x |
| `wells_dist100ars_model` | 1.01x | 19.1k | 0.363 | 2.4 | 6.6x |
| `i320_pois_trunc_ub` | 1.01x | 21.9k | 0.578 | 2.88 | 4.98x |
| `wells_daae_c_model` | 1.01x | 17.6k | 0.377 | 2.58 | 6.83x |
| `s2_mi_trunc_lb` | 1.01x | 37.9k | 0.21 | 3.52 | 16.7x |
| `s2_ar_cov` | 1.01x | 11.2k | 0.0866 | 6.2 | 71.6x |
| `cm_betadiscrete` | 1x | 9.91k | 22.9 | 26.3 | 1.15x |
| `s2_wiener` | 1x | 14.7k | 0.388 | 2.87 | 7.4x |
| `s2_cosy` | 0.995x | 14.7k | 0.102 | 5.53 | 54.4x |
| `sw_cumulative_cs` | 0.991x | 22.3k | 1.41 | 4.99 | 3.55x |
| `sw_cumulative` | 0.985x | 9.89k | 0.227 | 2.82 | 12.4x |
| `i320_pois_trunc_both` | 0.982x | 21.9k | 0.723 | 3.27 | 4.52x |
| `one_comp_mm_elim_abs` | 0.979x | 18.9k | 8.92 | 10.9 | 1.22x |
| `cm_ddm` † | 0.97x | 5k | 848 | 824 | 0.973x |
| `s2_fcor` | 0.97x | 9.87k | 0.147 | 3.93 | 26.7x |
| `hmm_drive_0` | 0.964x | 24.5k | 3.65 | 7.49 | 2.05x |
| `i320_gp_matern32` | 0.918x | 133k | 4.2 | 10.3 | 2.45x |
| `aalto_gpareto` | 0.788x | 9.37k | 0.0165 | 1.81 | 110x |
| `cm_invgaussian` † | 0.756x | 5k | 0.824 | 4.42 | 5.36x |
<!--/gen-->

<!--gen:benchmark_table_notes-->
Run `5167a7b02ed6d888` (2026-11-13).

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.
<!--/gen-->

<!--gen:benchmark_failures-->
### Fails the numerical check

These models are left out of every speedup on this page. The gate is 1e-9 scaled error ([details](#numerical-gate)).

- `ctsem_ctsm_post4_100`: point 4: density/gradient mismatch: scaled error 1.38e-09
<!--/gen-->

### CmdStan with loop vectorization

stanc3 can vectorize loops at `--O1`, and CmdStan leaves that off by default.
This comparison turns it on for CmdStan and leaves Stanli unchanged
([how it is built](#loop-vectorization-comparison)). The per-model table is in the
[evidence README](../output/corpus-performance-vectorized/README.md).

<!--gen:benchmark_vectorized_summary-->
With loop vectorization on, the median model's gradient is 1.97x faster in Stanli than in CmdStan, against 2.17x for default CmdStan. The speedup changes by more than 10% on 137 of 346 models: it shrinks on 127 and grows on 10. These 10 models change most, in how many times faster Stanli's gradient is:

| Model | Default CmdStan | Vectorized CmdStan |
| --- | ---: | ---: |
| `radon_pooled` | 8.52x | 2.05x |
| `sw_nonlinear` | 5.35x | 2.22x |
| `dogs` | 7.76x | 3.4x |
| `blr` | 2.79x | 1.32x |
| `s2_nl_noloop` | 4.16x | 2.02x |
| `ch14_m14_5` | 4.56x | 2.23x |
| `logistic_regression_rhs` | 2.37x | 1.17x |
| `election88_full` | 3.96x | 1.97x |
| `sw_dist_sigma` | 3.76x | 1.96x |
| `ch14_m14_4x` | 3.88x | 2.03x |
<!--/gen-->

## Fast mode

<!--gen:fast_lead-->
These figures are from 2026-11-13 (Stanli source `b4247512`). [Fast
mode](fast-mode.md) used the same protocol, inputs and binaries as the default
run, with models compiled and run in fast mode (`corpus_bench.py
--fast-math`).
<!--/gen-->

The median model is **<!--gen:fast_median-->1.03x<!--/gen--> faster** than in default mode. Of the
models with paired gradient timings (<!--gen:fast_population-->346<!--/gen-->), <!--gen:fast_ge_two-->82<!--/gen--> are at least 2x faster, <!--gen:fast_ge_ten-->40<!--/gen--> at least 10x
and <!--gen:fast_ge_hundred-->8<!--/gen--> at least 100x. The large gains are models whose likelihood repeats over
the data: fast mode evaluates it once per distinct row or group. The geometric
mean is <!--gen:fast_geomean-->1.90x<!--/gen--> faster.

- Fast mode beats CmdStan on <!--gen:fast_beats_fast-->338<!--/gen--> models, default mode on <!--gen:fast_beats_default-->335<!--/gen-->.
- In fast mode, <!--gen:fast_slowest-->the slowest model, `s2_multinomial`, is 1.05x slower.<!--/gen-->

The largest gains, gradient time in microseconds:

<!--gen:fast_top_table-->
| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 41.5 | 0.0754 | 551x faster |
| `nes` | 18.4 | 0.0727 | 253x faster |
| `logearn_interaction_z` | 7.96 | 0.0509 | 156x faster |
| `logearn_interaction` | 7.94 | 0.0511 | 156x faster |
| `logearn_logheight_male` | 5.69 | 0.0446 | 128x faster |
| `logearn_height_male` | 5.62 | 0.0448 | 126x faster |
| `diamonds` | 32.7 | 0.306 | 107x faster |
| `ch12_m12_4` | 66.5 | 0.635 | 105x faster |
<!--/gen-->

The per-model fast-mode table, raw observations and build identity are in
[`output/corpus-performance-fast/`](../output/corpus-performance-fast/README.md).

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

`ctsem_ctsm_post4_100` fails the check at one of its points with a scaled
error of about 1.4e-9, above the 1e-9 gate. Its reference run timed out, so it
uses the fixed points, and this run is its first measurement.
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
the same model.

A fallback model uses 5,000 gradients. A missing setup component leaves the
estimate blank and keeps the gradient result.

### CmdStan on macOS

On macOS, CmdStan's makefile links TBB's malloc replacement
(`tbbmalloc_proxy`) into every model. The benchmark's CmdStan driver links
only `tbb`, as CmdStan does on Linux, and that makes CmdStan faster than a
default Mac build on allocation-heavy models. In a spot check the default build
was 1.7x slower on `ch12_m12_4` and 1.5x slower on `gpcm_latent_reg_irt`.

### Loop-vectorization comparison

Stock pinned stanc3 leaves loop vectorization off at `--O1`. The comparison turns
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

### Runs

<!--gen:benchmark_runs-->
CmdStan stanc and build times for 347 of 347 models were reused from the build cache and not remeasured.

Stanli timings in the loop-vectorization comparison are from the default run. The CmdStan timings with loop vectorization are reused from run `937801ca404cae01` (2026-11-13). `sw_acat`, `sw_acat_cs`, `sw_ar`, `sw_arma`, `sw_asymlaplace`, `sw_bernoulli`, `sw_beta`, `sw_binomial`, `sw_categorical`, `sw_cens`, `sw_cratio`, `sw_cratio_cs`, `sw_cumulative`, `sw_cumulative_cs`, `sw_dist_sigma`, `sw_exgaussian`, `sw_gamma`, `sw_gaussian`, `sw_gp`, `sw_hurdle_gamma`, `sw_hurdle_lognormal`, `sw_hurdle_pois`, `sw_lognormal`, `sw_ma`, `sw_me`, `sw_mi`, `sw_mixture`, `sw_mono`, `sw_mv_norescor`, `sw_mv_rescor`, `sw_negbinomial`, `sw_nonlinear`, `sw_poisson`, `sw_re_bern`, `sw_re_gauss`, `sw_re_negbin`, `sw_re_pois`, `sw_re_slope`, `sw_se`, `sw_skewnormal`, `sw_spline_s`, `sw_spline_t2`, `sw_sratio`, `sw_student`, `sw_trunc`, `sw_vonmises`, `sw_weibull`, `sw_weights`, `sw_zi_binomial`, `sw_zi_negbin`, `sw_zi_poisson` is from run `927990d5d0dcee9d` (2026-11-14). `i320_gp_expquad`, `i320_gp_matern32`, `i320_mi_nhanes`, `i320_pois_trunc_both`, `i320_pois_trunc_ub`, `i320_sratio_cs`, `i320_sratio_plain` is from run `729e8433e40e69a9` (2026-11-15). `i319_gauss_re`, `i319_negbin_fixed`, `i319_negbin_re`, `i319_pois_fixed`, `i319_pois_re`, `i319_pois_re2` is from run `ada72c9096a10e01` (2026-11-15). `s2_cumulative_cauchit`, `s2_cumulative_cloglog`, `s2_cumulative_probit`, `s2_custom_vint`, `s2_custom_vreal` is from run `68c1ac87a5835bac` (2026-11-15).
<!--/gen-->

<a id="reproduction"></a>

## Appendix: reproduction

| Item | Configuration |
| --- | --- |
| Scope | All <!--gen:benchmark_models-->347<!--/gen--> application models, `--corpus all` |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled |
| Measured source | <!--gen:benchmark_source-->`b4247512ca87f95da9fc9d088fba956f91c34e09`<!--/gen-->; tracked differences and executable hashes in manifests |
| Default reference compiler | Stock stanc3 v2.40.0 (`STANC3_UPSTREAM_SHA`), no additional flags; Stanli compiles with the seantalts/stanc3 fork (`STANC3_SRC_SHA`) |
| CmdStan | <!--gen:benchmark_cmdstan-->2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`<!--/gen--> |
| Stan Math | <!--gen:benchmark_stan_math-->`5252d51d47c1d5e78005fc043ad996fad6dd8da8`<!--/gen--> |
| Host | <!--gen:benchmark_host-->Apple M3 Ultra, macOS-26.6.2-arm64-arm-64bit, 32 logical CPUs, 96 GiB RAM<!--/gen--> |

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
`tools/gen_benchmark_points.py` reruns the reference runs for every model, or
only for `--models NAME,...` or `--missing` (models with no entry or with
changed source or data), and merges the results into the committed points file.
Use `libstanli.so` on Linux; `--jobs` and `--timeout` set the parallelism and
the per-model limit. The harness reads the file by
default through `--points-file` and refuses a model whose source or data no
longer matches.
CmdStan binaries are built once and reused from a content-addressed cache,
`$STANLI_BENCH_CACHE` or `~/.cache/stanli/corpus-bench`; `--build-cache DIR`
overrides it and `--no-build-cache` builds inside the run directory.
`--build-jobs N` (default 8) sets how many gradient driver builds run at once.
The harness refuses a CmdStan checkout that differs from `deps/pins.env` in its CmdStan,
Stan or Stan Math head, its tracked files or its `bin/stanc`; `--allow-unpinned-cmdstan`
overrides that and the manifest records it.
`--stanc` (alias `--cmdstan-stanc`) and `--stancflags` choose CmdStan's
compiler; Stanli uses the `stanli-vectorize-probe` in `deps/stanc3/embed/<key>/`. `--resume`
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
is the loop-vectorization comparison: when CmdStan, its toolchain and the patched
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

<a id="benchmarking-a-change"></a>

## Appendix: benchmarking a change

The corpus run above publishes results. These notes cover measuring one change
against the branch it targets.

### Compiler-pipeline regression checks

A change to the compiler pipeline must benchmark both the current default
branch and the candidate before merge. Run the full inventory
(`--corpus all`, no `--filter`) with the shipped configuration for both
revisions, on matched hardware, compiler, build mode, threads, inputs and
settings, with separate output paths.

Compare each model's source-to-MIR time, preparation time, warm gradient and
total sampling estimate as `100 * (candidate / baseline - 1)`. A model's
slowdown is major when its total sampling estimate grows by 2% or 5 ms,
whichever is greater. Show per-model and per-phase results, since an aggregate
win can hide a regression. Missing, failed or timed-out
observations stay visible and cannot count as a pass, and every existing
numerical check still applies.

Shared CI hosts can be noisier than that. Investigate a threshold crossing with a
separate experiment on an idle host: at least six alternating fresh-process
baseline/candidate pairs for the affected phases, all samples kept, with
identical-binary controls when needed. Do not rerun until a favorable sample
appears. Fix a confirmed regression before merge. The PR lists both revision
SHAs, build identities, exact commands, coverage, the phase comparison and
links to the raw results.

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

### Corpus benchmark build cache

An entry is keyed by the `.stan` bytes, the stanc binary and flags, the
gradient driver and timer sources, the CmdStan checkout, library, compiler and
environment identity, and the CmdStan path, so worktrees share entries. It
holds the model header, the gradient driver, the ordinary executable and the
original stanc and build durations; Stanli compilation, preparation and every
gradient are always measured fresh. The run directory records the cache
location and each model's hit or miss in `build-prepass.jsonl` and
`*.result.json`.
