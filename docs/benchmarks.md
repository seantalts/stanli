# Benchmarks

## TL;DR

- For the median of <!--gen:benchmark_models-->363<!--/gen--> application models, Stanli computes
  gradients **<!--gen:corpus_median-->2.15x<!--/gen--> faster** than CmdStan.
- For the median model, [total sampling](#full-corpus) is
  **<!--gen:corpus_total_median-->23x<!--/gen--> faster**, mostly because CmdStan builds C++ code
  for each model. Stanli prepares the structure that depends on the data
  once, then reuses that plan and its buffers
  ([how it works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)).
- Stanli is as fast as CmdStan, or faster, on <!--gen:corpus_at_par-->349<!--/gen--> of the <!--gen:corpus_n_grad-->362<!--/gen--> models
  that pass the numerical check.
- Regressions, GLMs and IRT models gain the most. Their observations are
  independent, so Stanli can batch them into vector operations
  ([loop recovery](how-it-works.md#recovering-vector-operations-from-scalar-loops),
  [optimization details](../runtime/src/OPTIMIZATIONS.md)).
- Models with dense linear algebra, ODE solvers or sequential recurrences
  spend their time in Stan Math in both engines. Stanli is about as fast as
  CmdStan on these models, or slower.
- [Fast mode](#fast-mode) is <!--gen:fast_cmdstan_fast-->4.27x<!--/gen--> faster than CmdStan in geometric mean.

All timings were measured on <!--gen:benchmark_hardware-->Apple M3 Ultra<!--/gen-->.

<a id="full-corpus"></a>

## Full corpus results

Every application model runs with the same settings. The language-conformance
fixtures belong to the [numerical corpus](corpus-status.md) and are not timed
here. CmdStan uses its default compiler settings. The `CmdStan / Stanli
gradient` column shows how many times faster Stanli's gradient is. The rows
are sorted by this column.

Total sampling is an estimate. It adds each engine's setup time to the time
for the gradients of a run with 1,000 warmup iterations and 1,000 draws. The
gradient count comes from a reference run ([method](#total-sampling-estimate)).
The last column shows how many times faster Stanli's total sampling is.

<!--gen:benchmark_table-->
| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ch12_m12_4` | 31.9x | 18.6k | 1.35 | 44.3 | 32.9x |
| `gpcm_latent_reg_irt` | 12.6x | 69k | 7.95 | 104 | 13.1x |
| `grsm_latent_reg_irt` | 12.1x | 63.1k | 4.19 | 54.4 | 13x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11x | 47.3k | 1.28 | 15.5 | 12.2x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.8x | 68.5k | 1.87 | 21.9 | 11.7x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 50.6k | 2.32 | 25 | 10.8x |
| `radon_county_intercept` | 9.9x | 62.8k | 2.87 | 29.6 | 10.3x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.89x | 81.2k | 3.8 | 39.4 | 10.4x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.89x | 52.9k | 2.45 | 25.5 | 10.4x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.4x | 102k | 4.84 | 47.3 | 9.79x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.36x | 77.7k | 5.05 | 49 | 9.71x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.2x | 100k | 6.63 | 62.5 | 9.42x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.19x | 59.7k | 3.1 | 30 | 9.7x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.11x | 125k | 6.54 | 61.7 | 9.43x |
| `aalto_poisson_hurdle` | 8.88x | 6.92k | 0.0622 | 2.31 | 37.2x |
| `radon_pooled` | 8.56x | 9.33k | 0.39 | 4.68 | 12x |
| `s2_custom_vreal` | 7.9x | 10.2k | 0.0128 | 2.34 | 183x |
| `dogs` | 7.81x | 20.7k | 0.199 | 4.06 | 20.4x |
| `rats_model` | 7.61x | 54.5k | 0.0474 | 2.14 | 45.1x |
| `arK` | 7.32x | 59.1k | 0.109 | 2.53 | 23.2x |
| `ch12_m12_3_alt` | 7.15x | 11k | 0.0361 | 1.58 | 43.8x |
| `ch12_m12_3` | 6.9x | 10.8k | 0.0383 | 1.6 | 41.6x |
| `ch16_m16_1` | 6.78x | 301k | 1.22 | 10.2 | 8.31x |
| `M0_model` | 6.51x | 7.46k | 0.028 | 1.7 | 60.9x |
| `Mt_model` | 5.78x | 11k | 0.0488 | 2.68 | 54.9x |
| `sw_nonlinear` | 5.47x | 10.9k | 0.0135 | 2.74 | 203x |
| `dogs_log` | 5.27x | 12.1k | 0.119 | 2.73 | 23x |
| `ctsem_ctsm_400` † | 5.25x | 5k | 20 | 198 | 9.91x |
| `ctsem_ctsm` | 5.05x | 1.08M | 434 | 2.29e+03 | 5.27x |
| `ch09_m5_8s` | 4.96x | 1.37M | 0.619 | 4.79 | 7.74x |
| `ch09_m5_8s2` | 4.73x | 1.11M | 0.523 | 4.23 | 8.09x |
| `s2_invgaussian` | 4.72x | 16.3k | 0.0159 | 2.55 | 160x |
| `ch14_m14_5` | 4.51x | 12k | 0.0357 | 1.88 | 52.6x |
| `logmesquite_logvas` | 4.38x | 92.3k | 0.0484 | 2.35 | 48.7x |
| `ch15_m15_1` | 4.36x | 30.7k | 0.0191 | 2.21 | 116x |
| `s2_cumulative_cauchit` | 4.35x | 20.7k | 0.028 | 2.56 | 91.5x |
| `logmesquite` | 4.28x | 56.4k | 0.0336 | 2.02 | 60.1x |
| `mesquite` | 4.27x | 160k | 0.0789 | 2.17 | 27.5x |
| `ch15_m15_2` | 4.19x | 32.4k | 0.0262 | 2.48 | 94.7x |
| `s2_nl_noloop` | 4.18x | 52.6k | 0.0332 | 2.87 | 86.4x |
| `s2_car_icar` | 4.12x | 1.7M | 0.703 | 6.13 | 8.71x |
| `s2_logistic_normal` | 4.1x | 13.6k | 0.0799 | 5.57 | 69.7x |
| `nes` | 4.05x | 90.8k | 1.68 | 8.7 | 5.17x |
| `logmesquite_logva` | 4.04x | 58.7k | 0.0236 | 2.02 | 85.8x |
| `election88_full` | 3.95x | 429k | 104 | 413 | 3.96x |
| `logmesquite_logvash` | 3.9x | 93.7k | 0.047 | 2.29 | 48.7x |
| `logmesquite_logvolume` | 3.89x | 12.4k | 0.00947 | 1.8 | 191x |
| `ch14_m14_4x` | 3.86x | 9.05k | 0.0221 | 1.76 | 79.8x |
| `Mth_model` | 3.83x | 33.9k | 0.894 | 6.8 | 7.61x |
| `sw_spline_s` | 3.82x | 37.6k | 0.0271 | 3.36 | 124x |
| `multinma_ndmm_mspline` | 3.79x | 35.4k | 0.561 | 27.7 | 49.4x |
| `sw_me` | 3.78x | 57.9k | 0.0397 | 3.06 | 77.2x |
| `s2_s_cc` | 3.73x | 30.5k | 0.0206 | 3.19 | 155x |
| `ch14_m14_4` | 3.72x | 9.05k | 0.0217 | 1.76 | 81.2x |
| `sw_dist_sigma` | 3.7x | 10.7k | 0.0159 | 2.68 | 169x |
| `s2_dist_sigma_re` | 3.67x | 29.4k | 0.0294 | 3.83 | 130x |
| `s2_me2_nomecor` | 3.66x | 50.4k | 0.0553 | 3.41 | 61.7x |
| `s2_nlf` | 3.61x | 9.42k | 0.0161 | 2.82 | 175x |
| `kilpisjarvi` | 3.6x | 1.12M | 0.23 | 2.34 | 10.2x |
| `aalto_grp_aov` | 3.58x | 12.5k | 0.00858 | 2.03 | 237x |
| `ch09_m9_1` | 3.58x | 11.3k | 0.015 | 2.04 | 136x |
| `sw_weights` | 3.57x | 9.98k | 0.0138 | 2.33 | 169x |
| `ch09_m9_1_chains4` | 3.56x | 11.3k | 0.0154 | 2.04 | 133x |
| `kidscore_interaction_c2` | 3.56x | 17.7k | 0.0544 | 1.95 | 35.8x |
| `kidscore_interaction_z` | 3.55x | 24k | 0.0707 | 2.08 | 29.4x |
| `kidscore_interaction` | 3.5x | 178k | 0.48 | 3.46 | 7.2x |
| `kidscore_interaction_c` | 3.49x | 15.2k | 0.0471 | 1.94 | 41.1x |
| `s2_gp_approx` | 3.48x | 34k | 0.0311 | 4.15 | 134x |
| `s2_shifted_lognormal` | 3.48x | 15.1k | 0.0159 | 2.79 | 176x |
| `kidscore_mom_work` | 3.47x | 40.5k | 0.115 | 2.08 | 18x |
| `s2_cox_cens` | 3.45x | 22.1k | 0.0301 | 4.03 | 134x |
| `s2_mm` | 3.43x | 26.6k | 0.0269 | 3.57 | 133x |
| `ch16_m16_4` | 3.43x | 35.3k | 0.0629 | 2.03 | 32.3x |
| `sw_se` | 3.41x | 7.2k | 0.0112 | 2.33 | 208x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.41x | 1.03M | 6.36 | 25.5 | 4.01x |
| `ch15_m15_5` | 3.41x | 21.5k | 0.0176 | 2.23 | 126x |
| `s2_mm_weights` | 3.4x | 27.8k | 0.0274 | 3.57 | 130x |
| `logearn_logheight_male` | 3.4x | 715k | 4.01 | 15.3 | 3.81x |
| `logearn_interaction` | 3.39x | 322k | 2.52 | 10.4 | 4.11x |
| `logearn_interaction_z` | 3.38x | 22k | 0.181 | 2.5 | 13.8x |
| `ch15_m15_9` | 3.36x | 11.3k | 0.0337 | 2.01 | 59.8x |
| `logearn_height_male` | 3.36x | 189k | 1.08 | 5.29 | 4.92x |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.35x | 17.3k | 0.0097 | 2.27 | 234x |
| `s2_car` | 3.35x | 30.6k | 0.0294 | 3.27 | 111x |
| `GLMM1_model` | 3.34x | 32.8k | 0.332 | 3.31 | 9.99x |
| `sw_lognormal` | 3.33x | 8.28k | 0.0113 | 2.55 | 226x |
| `s2_car_esicar` | 3.31x | 28.9k | 0.0282 | 3.4 | 121x |
| `sw_spline_t2` | 3.26x | 38.1k | 0.045 | 3.88 | 86.4x |
| `ch15_m15_8` | 3.26x | 10.7k | 0.0322 | 1.84 | 57.2x |
| `kidscore_momhsiq` | 3.25x | 90.5k | 0.186 | 2.32 | 12.4x |
| `s2_gp_by_approx` | 3.24x | 37.2k | 0.0517 | 5.01 | 97x |
| `s2_t2_by` | 3.22x | 36.4k | 0.0693 | 4.89 | 70.7x |
| `s2_s_by` | 3.22x | 70.7k | 0.114 | 5.08 | 44.6x |
| `hdbayes_pwe_npp` | 3.21x | 28.1k | 0.0968 | 3.78 | 39.1x |
| `i319_gauss_re` | 3.17x | 41.3k | 0.0696 | 3.94 | 56.7x |
| `s2_gr_by` | 3.16x | 33.2k | 0.0265 | 3.85 | 145x |
| `radon_county` | 3.14x | 48.8k | 1.3 | 5.93 | 4.57x |
| `aalto_grp_prior_mean` | 3.13x | 19.3k | 0.0105 | 2.14 | 204x |
| `cm_exwald` | 3.12x | 68k | 2.97 | 12.5 | 4.23x |
| <code>ecoensemble_</code><br><code>hierarchical_withdrivers</code> | 3.05x | 163k | 14.9 | 65.5 | 4.38x |
| `s2_cox` | 3.03x | 30.6k | 0.0341 | 3.08 | 90.2x |
| `sw_re_gauss` | 3.02x | 29.1k | 0.0244 | 3.46 | 142x |
| <code>ecoensemble_</code><br><code>hierarchical_</code><br><code>withdrivers_conj</code> | 3.01x | 192k | 17.9 | 73.6 | 4.12x |
| `ch15_m15_6` | 3.01x | 15.6k | 0.0116 | 1.92 | 166x |
| `sw_acat` | 2.99x | 12.2k | 0.602 | 4.46 | 7.41x |
| `aalto_lin_std` | 2.95x | 11k | 0.00991 | 2.28 | 230x |
| `aalto_lin` | 2.92x | 18.2k | 0.00987 | 2.04 | 206x |
| `pilots` | 2.91x | 551k | 0.186 | 2.69 | 14.4x |
| `s2_mi_lognormal` | 2.9x | 14.8k | 0.0195 | 3.47 | 178x |
| `earn_height` | 2.89x | 150k | 0.566 | 3.33 | 5.88x |
| `s2_cumulative_cloglog` | 2.88x | 20.3k | 0.0332 | 2.53 | 76.1x |
| `eight_schools_centered` | 2.87x | 32.4k | 0.0114 | 1.92 | 169x |
| `kidscore_momiq` | 2.87x | 60.1k | 0.0896 | 1.87 | 20.9x |
| `logearn_height` | 2.86x | 138k | 0.53 | 3.11 | 5.87x |
| `log10earn_height` | 2.86x | 143k | 0.545 | 3.14 | 5.76x |
| `sw_student` | 2.85x | 12.7k | 0.0159 | 2.71 | 170x |
| `bym2_offset_only` | 2.85x | 202k | 7.73 | 25.4 | 3.29x |
| `GLMM_Poisson_model` | 2.85x | 75.6k | 0.0526 | 3.02 | 57.5x |
| `sesame_one_pred_a` | 2.83x | 15.5k | 0.0191 | 1.62 | 84.5x |
| `sw_hurdle_lognormal` | 2.83x | 12.5k | 0.0229 | 2.42 | 106x |
| `s2_hurdle_cumulative` | 2.81x | 11.2k | 0.0396 | 3.06 | 77.3x |
| `blr` | 2.8x | 32.4k | 0.0193 | 2.14 | 111x |
| `aalto_grp_prior_mean_var` | 2.8x | 59.3k | 0.0236 | 3.03 | 128x |
| `Mtbh_model` | 2.8x | 34.3k | 0.562 | 5.77 | 10.3x |
| `dogs_nonhierarchical` | 2.79x | 56.4k | 0.894 | 8.82 | 9.86x |
| `s2_gr_student` | 2.79x | 52.2k | 0.0451 | 4.14 | 91.6x |
| `GLM_Poisson_model` | 2.75x | 29.5k | 0.019 | 2.31 | 122x |
| `sw_acat_cs` | 2.73x | 18.3k | 1.17 | 7.01 | 5.98x |
| `ch13_m13_4nc` | 2.7x | 75.5k | 0.66 | 4.07 | 6.16x |
| `sw_gamma` | 2.7x | 9.45k | 0.0156 | 2.6 | 166x |
| `kidscore_momhs` | 2.69x | 24.8k | 0.0415 | 1.73 | 41.7x |
| `s2_mmc` | 2.69x | 35.4k | 0.0601 | 6.06 | 101x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo2</code> | 2.67x | 34.3k | 0.337 | 13 | 38.7x |
| `s2_gev` | 2.66x | 12.2k | 0.0338 | 2.68 | 79.2x |
| `hierarchical_gp` | 2.63x | 352k | 5.93 | 24.4 | 4.11x |
| `cm_geg` | 2.63x | 54k | 1.37 | 6.53 | 4.77x |
| `accel_gp` | 2.6x | 1.5M | 5.41 | 18.7 | 3.46x |
| `sw_hurdle_pois` | 2.59x | 10.4k | 0.0273 | 2.21 | 81.1x |
| `oncobayes2_blrm_exnex_sa` | 2.58x | 25.3k | 0.139 | 12.5 | 90.2x |
| `accel_splines` | 2.56x | 1.86M | 6.93 | 21.4 | 3.09x |
| `ch12_m12_7` | 2.55x | 48.6k | 50.4 | 130 | 2.59x |
| `losscurve_sislob` | 2.54x | 21.3k | 0.0339 | 3.18 | 93.9x |
| `cm_exgaussian` | 2.53x | 14k | 0.224 | 3.25 | 14.5x |
| `ch12_m12_5` | 2.53x | 37.4k | 40 | 103 | 2.58x |
| `s2_me2` | 2.51x | 71.6k | 0.13 | 5.99 | 46.1x |
| `s2_sar` | 2.49x | 10.9k | 0.0492 | 3 | 61.1x |
| `s2_sar_error` | 2.48x | 11.4k | 0.053 | 3.06 | 57.7x |
| `ldaK5` † | 2.47x | 5k | 12.6 | 33 | 2.61x |
| `ch11_m11_10` | 2.45x | 10.7k | 0.00962 | 2.24 | 233x |
| `seeds_centered_model` | 2.42x | 41.5k | 0.0359 | 2.75 | 76.7x |
| `logistic_regression_rhs` | 2.41x | 137k | 6.08 | 18.4 | 3.03x |
| `i319_pois_re2` | 2.39x | 173k | 0.824 | 5.97 | 7.24x |
| `ch14_m14_3` | 2.38x | 65.2k | 0.596 | 6.65 | 11.1x |
| `s2_rate` | 2.37x | 7.8k | 0.0136 | 2.46 | 181x |
| `ch15_m15_3` | 2.33x | 7.74k | 0.108 | 1.75 | 16.3x |
| `sw_re_bern` | 2.32x | 27.4k | 0.0273 | 3.54 | 130x |
| `aalto_lin_std_t` | 2.28x | 13k | 0.0112 | 2.52 | 225x |
| `normal_mixture` | 2.28x | 7.96k | 0.307 | 2.02 | 6.58x |
| `ch09_m9_4` | 2.27x | 1.55M | 0.216 | 1.92 | 8.89x |
| `ch15_m15_4` | 2.27x | 8.81k | 0.102 | 1.74 | 17x |
| `Mh_model` | 2.27x | 53.8k | 0.916 | 4.34 | 4.74x |
| `sw_skewnormal` | 2.26x | 15.5k | 0.0258 | 2.85 | 111x |
| `dogs_hierarchical` | 2.26x | 9.37k | 0.174 | 2.03 | 11.7x |
| `ch09_m9_5` | 2.26x | 398k | 0.0608 | 1.57 | 25.8x |
| `ch11_m11_5` | 2.25x | 29.3k | 0.201 | 2.48 | 12.3x |
| `extra_hurdle_poisson` | 2.24x | 8.4k | 0.00935 | 1.47 | 157x |
| `s2_mv_shared_re` | 2.24x | 36.8k | 0.0637 | 6.17 | 96.8x |
| `dugongs_model` | 2.24x | 33.4k | 0.0228 | 2.01 | 88.1x |
| `ldaK2` | 2.23x | 19.8k | 0.997 | 4.78 | 4.79x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo3</code> | 2.23x | 33.5k | 1.09 | 14.6 | 13.4x |
| `ch09_m9_3` | 2.22x | 10.8k | 0.00722 | 1.41 | 195x |
| `sw_vonmises` | 2.21x | 730k | 0.514 | 3.64 | 7.08x |
| `ch09_m9_2` | 2.21x | 46.7k | 0.00988 | 1.41 | 143x |
| `i320_mi_nhanes` | 2.21x | 312k | 0.278 | 4.16 | 14.9x |
| `ch13_m13_6` | 2.2x | 36.1k | 0.255 | 2.94 | 11.5x |
| `sw_re_slope` | 2.2x | 53.4k | 0.0674 | 5.88 | 87.2x |
| `ch13_m13_4` | 2.19x | 34.4k | 0.245 | 2.92 | 11.9x |
| `hier_2pl` | 2.18x | 59.4k | 12 | 32.3 | 2.69x |
| `ch13_m13_4b` | 2.17x | 34.4k | 0.244 | 2.8 | 11.5x |
| `lsat_model` | 2.17x | 40.8k | 1.81 | 6.5 | 3.59x |
| `ch13_m13_7` | 2.16x | 19.9k | 0.00768 | 1.18 | 153x |
| `sw_zi_poisson` | 2.14x | 11.5k | 0.0238 | 2.2 | 92.4x |
| `2pl_latent_reg_irt` | 2.13x | 51.2k | 3.43 | 12.7 | 3.69x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.13x | 44.4k | 2.13 | 6.41 | 3.01x |
| `low_dim_gauss_mix` | 2.12x | 14k | 0.698 | 3.48 | 4.98x |
| `ch09_mp` | 2.11x | 38.9k | 0.00813 | 1.16 | 143x |
| `sw_re_pois` | 2.1x | 30k | 0.0336 | 3.4 | 101x |
| `s2_index_mi` | 2.07x | 18.6k | 0.0263 | 3.09 | 117x |
| `cm_bisa` | 2.07x | 27.9k | 0.643 | 4.43 | 6.89x |
| `cm_rdm` | 2.06x | 67.7k | 8.17 | 20.3 | 2.48x |
| `seeds_model` | 2.04x | 38.4k | 0.0332 | 2.49 | 75.2x |
| `prophet` | 2.03x | 1.66M | 61.7 | 129 | 2.09x |
| `ch13_m13_7nc` | 2.03x | 7.88k | 0.00695 | 1.21 | 174x |
| `multi_occupancy` | 2.02x | 98.9k | 3.05 | 11 | 3.63x |
| `sw_cens` | 2.01x | 10.8k | 0.0237 | 3.17 | 134x |
| `multinma_smoking_re` | 2x | 120k | 0.263 | 16.5 | 62.7x |
| `sw_exgaussian` | 2x | 25.1k | 0.0327 | 2.8 | 85.8x |
| `normal_mixture_k` | 1.99x | 243k | 47 | 95.7 | 2.04x |
| `seeds_stanified_model` | 1.99x | 42.2k | 0.0357 | 2.41 | 67.5x |
| `cm_lnr` | 1.98x | 41.9k | 4.63 | 12.6 | 2.72x |
| `cm_lognormal` | 1.98x | 68k | 2.67 | 8.58 | 3.21x |
| `rstanarm_glm_gaussian` | 1.98x | 14.9k | 0.248 | 23.6 | 95.3x |
| `irt_2pl` | 1.98x | 61.4k | 1.1 | 5.14 | 4.67x |
| `ch11_m11_4` | 1.95x | 16.5k | 0.109 | 2.17 | 19.9x |
| `ch13_m13_1` | 1.95x | 18.5k | 0.02 | 1.92 | 96.1x |
| `ch11_m11_6` | 1.94x | 16.7k | 0.0155 | 1.98 | 128x |
| `ch13_m13_5` | 1.93x | 18.9k | 0.126 | 2.45 | 19.4x |
| `ch11_m11_8` | 1.92x | 50.9k | 0.0219 | 1.89 | 86.5x |
| `cm_loggamma` | 1.91x | 226k | 6.68 | 15.9 | 2.38x |
| `i319_pois_re` | 1.9x | 192k | 0.705 | 4.96 | 7.03x |
| `s2_cens_interval` | 1.87x | 8.94k | 0.0242 | 2.61 | 108x |
| `soil_incubation` | 1.87x | 203k | 5 | 11.5 | 2.31x |
| `ch13_m13_2` | 1.86x | 22.9k | 0.0242 | 2.11 | 87.1x |
| `sw_hurdle_gamma` | 1.85x | 13.3k | 0.0354 | 2.42 | 68.2x |
| `lotka_volterra` | 1.84x | 75.1k | 2.48 | 7.76 | 3.13x |
| `sw_mi` | 1.83x | 15.1k | 0.0188 | 2.72 | 144x |
| `ch13_m13_3` | 1.83x | 27.8k | 0.0314 | 2.02 | 64.2x |
| `sw_arma` | 1.79x | 42.2k | 0.105 | 3.36 | 32.1x |
| `sw_ar` | 1.79x | 10.4k | 0.0264 | 3.12 | 118x |
| `sw_ma` | 1.78x | 11.1k | 0.0288 | 3.14 | 109x |
| `cm_logweibull` | 1.78x | 90.4k | 1.86 | 6.38 | 3.43x |
| `cm_choco` | 1.75x | 15.1k | 0.686 | 4.81 | 7.01x |
| `ch11_m11_11` | 1.75x | 143k | 0.0609 | 2.67 | 43.9x |
| `sw_beta` | 1.75x | 8.99k | 0.0249 | 2.74 | 110x |
| `sw_zi_binomial` | 1.74x | 10.2k | 0.0324 | 2.24 | 69.2x |
| `s2_threading` | 1.71x | 11.8k | 0.0121 | 2.22 | 183x |
| `cm_weibull` | 1.7x | 2.03M | 37.4 | 67.5 | 1.8x |
| `sw_asymlaplace` | 1.7x | 23.4k | 0.0563 | 2.44 | 43.4x |
| `wells_dist` | 1.69x | 26.5k | 0.663 | 2.8 | 4.23x |
| `cm_logstudent` | 1.69x | 2.02M | 30.7 | 54.5 | 1.78x |
| `cm_lba1` | 1.67x | 145k | 7.96 | 16.7 | 2.09x |
| `s2_mv_subset` | 1.66x | 12.1k | 0.0154 | 2.3 | 150x |
| `cm_gamma` | 1.66x | 2.01M | 41.6 | 72 | 1.73x |
| `cm_betagate` | 1.63x | 11.8k | 0.417 | 3.71 | 8.9x |
| `sw_gaussian` | 1.63x | 10.9k | 0.0116 | 2.11 | 183x |
| `cm_lnr_bench` | 1.63x | 40.1k | 10.2 | 20.4 | 2x |
| `s2_zi_beta` | 1.63x | 10.2k | 0.0499 | 2.53 | 50.7x |
| `s2_zoi_beta` | 1.62x | 12.1k | 0.0592 | 2.57 | 43.4x |
| `surgical_model` | 1.61x | 22.2k | 0.0164 | 2.23 | 136x |
| `ch11_m11_7` | 1.6x | 7.03k | 0.00864 | 1.77 | 205x |
| `cm_invweibull` | 1.59x | 87.1k | 2.01 | 6.29 | 3.13x |
| `sir` | 1.59x | 127k | 20.3 | 35.4 | 1.74x |
| `sw_mv_norescor` | 1.58x | 11.6k | 0.0165 | 2.3 | 139x |
| `s2_categorical_re` | 1.56x | 34.6k | 0.178 | 4.63 | 26x |
| `s2_com_poisson` | 1.55x | 42.5k | 0.849 | 3.73 | 4.39x |
| `GLM_Binomial_model` | 1.55x | 13.3k | 0.0193 | 2.3 | 119x |
| `s2_mixture_theta` | 1.54x | 80.4k | 0.451 | 4.06 | 9x |
| `cm_invgamma` | 1.54x | 79k | 1.8 | 5.87 | 3.27x |
| `s2_unstr` | 1.53x | 46.4k | 0.217 | 6.76 | 31.2x |
| `s2_frechet` | 1.53x | 33.2k | 0.0727 | 2.9 | 39.9x |
| `cm_lba2` | 1.51x | 140k | 13.4 | 24.1 | 1.8x |
| `Mb_model` | 1.51x | 11k | 0.411 | 2.82 | 6.86x |
| `gp_regr` | 1.51x | 8.82k | 0.0278 | 4.79 | 173x |
| `sw_re_negbin` | 1.5x | 30.3k | 0.067 | 3.68 | 54.9x |
| `sw_weibull` | 1.49x | 10.3k | 0.0284 | 2.76 | 96.9x |
| `gp_pois_regr` | 1.49x | 396k | 0.756 | 6.19 | 8.19x |
| `sw_zi_negbin` | 1.48x | 20.5k | 0.0709 | 2.37 | 33.5x |
| `s2_gp_by_gr` | 1.48x | 59.6k | 0.425 | 8.65 | 20.4x |
| `covid19imperial_v2` | 1.46x | 473k | 119 | 180 | 1.51x |
| `Rate_4_model` | 1.46x | 8.3k | 0.00788 | 1.39 | 177x |
| `s2_zi_asymlaplace` | 1.45x | 28.3k | 0.0959 | 2.54 | 26.5x |
| `kronecker_gp` | 1.44x | 2M | 323 | 469 | 1.45x |
| `covid19imperial_v3` | 1.44x | 473k | 118 | 176 | 1.49x |
| `hmm_example` | 1.43x | 25.1k | 0.501 | 3.85 | 7.68x |
| `Rate_1_model` | 1.43x | 4.69k | 0.00639 | 1.25 | 196x |
| `aalto_binom` | 1.43x | 5.11k | 0.00753 | 1.25 | 167x |
| `ch14_m14_2` | 1.43x | 135k | 2.26 | 9.82 | 4.35x |
| `i320_gp_expquad` | 1.42x | 52.4k | 0.568 | 7.37 | 13x |
| `aalto_bern` | 1.42x | 5.11k | 0.00787 | 1.44 | 183x |
| `ch12_m12_6` | 1.41x | 111k | 221 | 313 | 1.42x |
| `ch11_m11_9` | 1.41x | 5.08k | 0.00699 | 1.49 | 213x |
| `rstanarm_glmer_nb_count` | 1.4x | 221k | 1.73 | 17.7 | 10.2x |
| `ch15_m15_7` | 1.4x | 28k | 0.263 | 7.08 | 26.9x |
| `sw_mono` | 1.36x | 20.4k | 0.0405 | 2.85 | 70.4x |
| `aalto_binom2` | 1.35x | 7.7k | 0.00846 | 1.39 | 164x |
| `ch11_m_pois` | 1.34x | 6.85k | 0.00777 | 1.4 | 180x |
| `i319_negbin_re` | 1.34x | 66.7k | 0.661 | 4.81 | 7.27x |
| `sw_cratio_cs` | 1.34x | 13.7k | 1.07 | 5.01 | 4.69x |
| `ch12_m12_2` | 1.33x | 64.2k | 0.0642 | 3.14 | 48.9x |
| `sw_binomial` | 1.33x | 7.52k | 0.0225 | 2.42 | 107x |
| `Rate_2_model` | 1.33x | 7.31k | 0.00797 | 1.42 | 178x |
| `ch14_m14_8nc` | 1.33x | 263k | 0.743 | 5.98 | 8.04x |
| `aalto_binomb` | 1.32x | 5.61k | 0.00742 | 1.28 | 173x |
| `ch14_m14_1` | 1.31x | 37.6k | 0.202 | 6.71 | 33.2x |
| `Rate_5_model` | 1.31x | 5.33k | 0.00777 | 1.38 | 178x |
| `s2_beta_binomial` | 1.31x | 10.4k | 0.0523 | 2.6 | 49.8x |
| `i320_sratio_cs` | 1.31x | 20.4k | 1.6 | 5.96 | 3.71x |
| `ch12_m12_1` | 1.3x | 11.8k | 0.0198 | 2.21 | 111x |
| `sw_gp` | 1.3x | 47.3k | 0.908 | 7.16 | 7.89x |
| `sw_mixture` | 1.3x | 303k | 1.58 | 5.7 | 3.61x |
| `Rate_3_model` | 1.29x | 5.2k | 0.00738 | 1.28 | 173x |
| `s2_mo_simo_prior` | 1.29x | 21.3k | 0.0453 | 2.88 | 63.6x |
| <code>epinow2_estimate_</code><br><code>infections</code> | 1.28x | 87.8k | 3.53 | 17.5 | 4.95x |
| `ch14_m14_6x` | 1.28x | 21.6k | 1.86 | 8.51 | 4.58x |
| `nn_rbm1bJ10` | 1.26x | 2.03M | 319 | 407 | 1.28x |
| `ch14_m14_6` | 1.26x | 21.6k | 1.88 | 8.51 | 4.53x |
| `sw_sratio` | 1.26x | 10.4k | 0.631 | 3.21 | 5.09x |
| `ch14_m14_9` | 1.26x | 12.5k | 4.36 | 8.64 | 1.98x |
| <code>epinow2_estimate_</code><br><code>secondary</code> | 1.25x | 42.6k | 0.808 | 9.45 | 11.7x |
| `sw_cratio` | 1.23x | 10.4k | 0.618 | 3.17 | 5.12x |
| `i320_sratio_plain` | 1.22x | 15.4k | 0.937 | 3.72 | 3.97x |
| `ch14_m14_10` | 1.22x | 11.2k | 4.2 | 8.35 | 1.99x |
| `s2_custom_vint` | 1.21x | 10.5k | 0.0604 | 2.41 | 39.9x |
| `sw_bernoulli` | 1.21x | 8.63k | 0.0116 | 2.07 | 178x |
| `sw_mv_rescor` | 1.21x | 13.3k | 0.0873 | 5.25 | 60.1x |
| `ch14_m14_7` | 1.2x | 143k | 4.01 | 13.4 | 3.34x |
| `s2_cumulative_probit` | 1.19x | 13.7k | 0.0675 | 2.63 | 38.9x |
| `s2_discrete_weibull` | 1.19x | 31.8k | 0.116 | 2.41 | 20.7x |
| `ch14_m14_8` | 1.18x | 204k | 0.587 | 4.38 | 7.45x |
| `ch14_m14_11` | 1.18x | 12.7k | 7.27 | 11.6 | 1.6x |
| `sw_cumulative_cs` | 1.17x | 22.3k | 1.18 | 4.99 | 4.21x |
| `sw_poisson` | 1.16x | 9.85k | 0.0134 | 1.91 | 143x |
| `Survey_model` | 1.14x | 11.1k | 0.645 | 2.64 | 4.08x |
| `sw_categorical` | 1.12x | 11k | 0.0225 | 3.17 | 141x |
| `hmm_gaussian` | 1.12x | 63.9k | 16.4 | 22.1 | 1.35x |
| `s2_multinomial` | 1.11x | 12.9k | 0.13 | 3.02 | 23.3x |
| `s2_hurdle_negbin` | 1.1x | 15.2k | 0.108 | 2.39 | 22.1x |
| `nn_rbm1bJ100` † | 1.08x | 5k | 2.18e+03 | 2.36e+03 | 1.08x |
| `rstanarm_polr` | 1.07x | 15.2k | 0.227 | 4.92 | 21.7x |
| `s2_weights_trunc` | 1.07x | 25.1k | 0.171 | 2.64 | 15.5x |
| `sw_negbinomial` | 1.07x | 1.49M | 2.06 | 4.42 | 2.14x |
| `sw_trunc` | 1.07x | 27.2k | 0.178 | 2.59 | 14.6x |
| `hmm_drive_1` | 1.06x | 39.6k | 5.86 | 10 | 1.72x |
| `s2_dirichlet` | 1.05x | 12.6k | 0.168 | 3.38 | 20.1x |
| `wells_interaction_model` | 1.04x | 32.1k | 0.663 | 2.72 | 4.1x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0729 | 2.16 | 29.6x |
| `iohmm_reg` | 1.04x | 462k | 157 | 165 | 1.05x |
| `wells_dist100_model` | 1.03x | 11.5k | 0.205 | 2.19 | 10.7x |
| `bones_model` | 1.03x | 14.7k | 0.777 | 3.27 | 4.21x |
| `nes_logit_model` | 1.03x | 17.8k | 0.126 | 2.06 | 16.4x |
| `garch11` | 1.03x | 23.3k | 0.2 | 2.08 | 10.4x |
| `wells_dae_c_model` | 1.02x | 16.5k | 0.327 | 2.51 | 7.68x |
| `wells_daae_c_model` | 1.02x | 17.6k | 0.377 | 2.58 | 6.84x |
| `wells_dist100ars_model` | 1.02x | 19.1k | 0.365 | 2.4 | 6.58x |
| `mlumr_survival_relaxed` | 1.02x | 96.9k | 13.1 | 18.6 | 1.42x |
| `i320_pois_trunc_ub` | 1.02x | 21.9k | 0.574 | 2.89 | 5.03x |
| `wells_dae_model` | 1.02x | 24.2k | 0.504 | 2.55 | 5.06x |
| `wells_dae_inter_model` | 1.02x | 12.8k | 0.283 | 2.45 | 8.66x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.02x | 11.2k | 0.239 | 2.37 | 9.92x |
| `diamonds` | 1.01x | 1.51M | 49.6 | 52.5 | 1.06x |
| `s2_ar_cov` | 1.01x | 11.2k | 0.0859 | 6.2 | 72.2x |
| `arma11` | 1.01x | 11.2k | 0.0718 | 1.96 | 27.4x |
| `aalto_poisson_simple` | 1.01x | 5.02k | 0.0133 | 1.56 | 117x |
| `s2_wiener` | 1.01x | 14.7k | 0.361 | 2.85 | 7.89x |
| `i319_negbin_fixed` | 1.01x | 19.4k | 0.188 | 2.59 | 13.8x |
| `s2_mi_trunc_lb` | 1.01x | 37.9k | 0.211 | 3.51 | 16.7x |
| `cm_betadiscrete` | 1.01x | 9.91k | 22.9 | 26.4 | 1.15x |
| `s2_cosy` | 1x | 14.7k | 0.0997 | 5.53 | 55.5x |
| `one_comp_mm_elim_abs` | 1x | 18.9k | 8.87 | 11.1 | 1.25x |
| `sw_cumulative` | 0.981x | 9.89k | 0.228 | 2.82 | 12.3x |
| `i320_pois_trunc_both` | 0.973x | 21.9k | 0.733 | 3.27 | 4.46x |
| `hmm_drive_0` | 0.968x | 24.5k | 3.65 | 7.48 | 2.05x |
| `cm_ddm` † | 0.965x | 5k | 850 | 826 | 0.971x |
| `s2_fcor` | 0.939x | 9.87k | 0.149 | 3.93 | 26.3x |
| `rbioacc_odetk` | 0.937x | 20.4k | 5.5 | 10.5 | 1.91x |
| `hdbayes_lm_npp` | 0.932x | 88.7k | 0.232 | 6.48 | 27.9x |
| `i320_gp_matern32` | 0.93x | 133k | 4.18 | 10.3 | 2.47x |
| <code>mlumr_survival_mspline_</code><br><code>relaxed</code> | 0.9x | 63.3k | 7.62 | 13.7 | 1.8x |
| `rstanarm_glmer_bernoulli` | 0.868x | 57.3k | 1.27 | 15.4 | 12.2x |
| `beeguts_guts_sd` | 0.832x | 18.9k | 22.5 | 23.5 | 1.04x |
| `aalto_gpareto` | 0.77x | 9.37k | 0.0156 | 1.81 | 116x |
| `cm_invgaussian` † | 0.76x | 5k | 0.773 | 4.43 | 5.73x |
<!--/gen-->

<!--gen:benchmark_table_notes-->
Run `ca6d63336d009651` (2026-11-18).

† The reference run did not finish, so this model uses 5,000 gradients at the fixed points.
<!--/gen-->

<!--gen:benchmark_failures-->
### Fails the numerical check

These models are left out of every speedup on this page. The gate is 1e-9 scaled error ([details](#numerical-gate)).

- `ctsem_ctsm_post4_100`: point 4: density/gradient mismatch: scaled error 1.38e-09
<!--/gen-->

### CmdStan with loop vectorization

stanc3 can vectorize loops at `--O1`, but CmdStan leaves this off by default.
This comparison turns it on for CmdStan. Stanli is unchanged
([how it is built](#loop-vectorization-comparison)). The table for each model
is in the [evidence README](../output/corpus-performance-vectorized/README.md).

<!--gen:benchmark_vectorized_summary-->
With loop vectorization on, the median model's gradient is 1.94x faster in Stanli than in CmdStan, against 2.14x for default CmdStan. The speedup changes by more than 10% on 137 of 361 models: it shrinks on 128 and grows on 9. These 10 models change most, in how many times faster Stanli's gradient is:

| Model | Default CmdStan | Vectorized CmdStan |
| --- | ---: | ---: |
| `radon_pooled` | 8.56x | 2.1x |
| `sw_nonlinear` | 5.47x | 2.23x |
| `dogs` | 7.81x | 3.41x |
| `blr` | 2.8x | 1.33x |
| `s2_nl_noloop` | 4.18x | 2.02x |
| `logistic_regression_rhs` | 2.41x | 1.17x |
| `election88_full` | 3.95x | 1.94x |
| `ch14_m14_5` | 4.51x | 2.22x |
| `ch14_m14_4x` | 3.86x | 2.03x |
| `sw_dist_sigma` | 3.7x | 1.98x |
<!--/gen-->

## Fast mode

<!--gen:fast_lead-->
The fast mode run is from 2026-11-13 (Stanli source `b4247512`), with models
compiled and run in fast mode (`corpus_bench.py --fast-math`). The default
mode run is from 2026-11-18 (Stanli source `1e0a66ff`). The two runs used
different binaries, so the comparison below is between runs.
<!--/gen-->

| Fast mode | Result |
| --- | ---: |
| Models with paired gradient timings | <!--gen:fast_population-->346<!--/gen--> |
| Median speedup over default mode | <!--gen:fast_median-->1.02x<!--/gen--> |
| Geometric-mean speedup over default mode | <!--gen:fast_geomean-->1.89x<!--/gen--> |
| Models at least 2x faster than default mode | <!--gen:fast_ge_two-->82<!--/gen--> |
| Models at least 10x faster | <!--gen:fast_ge_ten-->39<!--/gen--> |
| Models at least 100x faster | <!--gen:fast_ge_hundred-->8<!--/gen--> |
| Models faster than CmdStan in fast mode | <!--gen:fast_beats_fast-->338<!--/gen--> |
| Models faster than CmdStan in default mode | <!--gen:fast_beats_default-->334<!--/gen--> |

The large gains come from models whose likelihood repeats over the data. Fast
mode evaluates such a term once per distinct row or group.
In fast mode, <!--gen:fast_slowest-->the slowest model, `oncobayes2_blrm_exnex_combo2`, is 1.14x slower.<!--/gen-->

The largest gains, gradient time in microseconds:

<!--gen:fast_top_table-->
| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 40.5 | 0.0754 | 537x faster |
| `nes` | 18.4 | 0.0727 | 253x faster |
| `logearn_interaction_z` | 7.86 | 0.0509 | 154x faster |
| `logearn_interaction` | 7.81 | 0.0511 | 153x faster |
| `logearn_height_male` | 5.64 | 0.0448 | 126x faster |
| `logearn_logheight_male` | 5.6 | 0.0446 | 126x faster |
| `ch12_m12_4` | 71.3 | 0.635 | 112x faster |
| `diamonds` | 32.8 | 0.306 | 107x faster |
<!--/gen-->

The fast-mode table for each model, the raw observations and the build
identity are in
[`output/corpus-performance-fast/`](../output/corpus-performance-fast/README.md).

<a id="how-we-measure"></a>

## Appendix: how we measure

Both engines evaluate the same model and data at the same unconstrained
parameter vectors. They use the sampling log density (`propto=true`) with the
Jacobian adjustment.

Each model has ten evaluation points and a gradient count. Both come from one
reference run per model. The reference run uses Stanli's NUTS sampler on the
real data with one chain, seed 1, 1,000 warmup iterations, 1,000 draws and
default adaptation, and it saves the warmup iterations. The run records
`n_leapfrog__` for every iteration, and each leapfrog step is one gradient.
The gradient count is the total over warmup and sampling. The file also
records the divergent and maximum-treedepth iterations.

The ten points are iterations of that run. They are chosen by systematic
selection over the cumulative gradient count of warmup and sampling together.
Iteration `i` is chosen for point `k` (0 to 9) when it is the first iteration
whose cumulative count reaches `(k + 0.5) / 10` of the total. So the parts of
the run where the sampler spends more gradients get more points. The sampler
already reports unconstrained draws, and the points are those values
unchanged.

A model uses fixed points instead if the reference run fails, times out after
30 minutes, or selects a point with a non-finite density or gradient. In
point 0 of the fixed points, coordinate `i` is
`0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. For points 1 to 9, each coordinate
is drawn uniformly from (-2, 2) by a fixed integer generator, seeded from the
point and coordinate. The vectors, the counts and the reason for each
fallback are in
[`benchmark-points.json.gz`](internal/artifacts/benchmark-points.json.gz).
Both engines read the same vectors. The tables mark fallback models with †.

### Gradient time

Each engine builds its model and evaluates every point once outside the
timed window. It then warms up for at least 20 ms at the first finite point.
At each finite point it calibrates a batch size, then measures batches of
gradients for at least 40 ms. The gradient time of a process is the mean of
its per-point times. Each process spends about 0.45 s measuring.

Each measurement runs in a fresh process, and only one engine runs at a time.
There are six pairs of processes, which alternate the order (Stanli first,
then CmdStan first). Each pair gives a ratio of CmdStan's mean to Stanli's
mean. The reported speedup is the median of the six ratios, so a value above
1x means that Stanli is faster.

The artifacts also keep the median absolute deviation (MAD) of the ratios and
of each engine's mean latency. Check the MAD before you trust a small
difference. Both drivers use the same timer,
[`benchmark_timer.hpp`](../tools/benchmark_timer.hpp).

### Numerical gate

For a pair to count, the two engines must agree at every point on the log
density and on every gradient component. They agree when the scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`.

If a point is non-finite in both engines, the harness drops the point and
records it. The mean then covers the remaining points. There is no accepted
pair if a point is non-finite in only one engine, if the widths are unequal,
or if a command fails. A model with no point that is finite in both engines
also has no accepted pair. All six pairs must keep the same points.

The harness applies this check while it runs. The published records keep each
point's timing and scaled error. They do not keep the density and gradient
values. The independent
[three-point corpus replay](../TESTING.md#comparison-with-cmdstan-on-complete-models)
checks correctness separately.

`ctsem_ctsm_post4_100` fails the check at one of its points with a scaled
error of about 1.4e-9, above the 1e-9 limit. Its reference run timed out, so it
uses the fixed points, and this run is its first measurement.
[TESTING.md](../TESTING.md#comparison-with-cmdstan-on-complete-models)
lists the numerical exceptions.

### Setup

Stanli setup is the time for the compiler process that turns source into
MIR, plus the time to prepare a bound executor from the MIR and JSON data.
The preparation time is the median of six fresh processes.

CmdStan setup is the time for stanc to translate the model, plus a fresh C++
build of the ordinary model. Common CmdStan dependencies and precompiled
headers are built in advance. CmdStan data loading and model initialization
are not counted, and neither is the build of the gradient driver.

Each compilation phase is timed once per model with normal filesystem caches.
So setup is neither a cold-cache time nor the time of a first fit inside a
Python or R session. CmdStan builds finish before any gradient timing starts.
The timed stanc and make steps run one at a time, with nothing else building.
Only the gradient driver builds run in parallel, `--build-jobs` at a time, and
their time is not published.

If a model's CmdStan inputs match an earlier build, the harness reuses that
build's stanc and make durations from the [build cache](#reproduction). The
record marks the event `cached` and names the run that measured it.

### Total sampling estimate

For each engine, the estimate is its setup time plus the model's gradient
count from the reference run, times its warm gradient time. The warm gradient
time is the mean over the ten points. Both engines use the same count.

The benchmark itself runs no sampler, so adaptation, tree building, generated
quantities and CSV output are left out. The count comes from Stanli's sampler.
The estimate assumes that CmdStan's sampler would take about as many
gradients on the same model.

A fallback model uses 5,000 gradients. If a setup component is missing, the
estimate is left blank and the gradient result is kept.

### CmdStan on macOS

On macOS, CmdStan's makefile links TBB's malloc replacement
(`tbbmalloc_proxy`) into every model. The benchmark's CmdStan driver links
only `tbb`, as CmdStan does on Linux. On models that allocate a lot of memory,
this makes CmdStan faster than a default Mac build. In a spot check, the
default build was 1.7x slower on `ch12_m12_4` and 1.5x slower on
`gpcm_latent_reg_irt`.

### Loop-vectorization comparison

The stock pinned stanc3 leaves loop vectorization off at `--O1`. The
comparison turns on that one setting and runs the resulting compiler with
`--O1`. So CmdStan gets both the O1 optimizations and loop vectorization. This
does not test loop vectorization alone, and it is not `--Oexperimental`.

The Stanli timings come from the default run. The CmdStan timings are reused
from earlier runs with the same CmdStan build, toolchain, inputs and
settings. Models that those runs lack are timed again. Each run checks its own
Stanli gradients against its own CmdStan gradients. The default run's Stanli
gradients are not compared with the reused CmdStan gradients. Because the two
sides come from separate runs, the ratio is CmdStan's mean gradient time
divided by Stanli's.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records the pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
is kept with the [evidence](../output/corpus-performance-vectorized/README.md).
So are the stock and patched MIR for a loop over a scalar normal likelihood.

### Runs

<!--gen:benchmark_runs-->
CmdStan stanc and build times for 363 of 363 models were reused from the build cache and not remeasured.

Stanli timings in the loop-vectorization comparison are from the default run. The CmdStan timings with loop vectorization are reused from run `937801ca404cae01` (2026-11-13). `sw_acat`, `sw_acat_cs`, `sw_ar`, `sw_arma`, `sw_asymlaplace`, `sw_bernoulli`, `sw_beta`, `sw_binomial`, `sw_categorical`, `sw_cens`, `sw_cratio`, `sw_cratio_cs`, `sw_cumulative`, `sw_cumulative_cs`, `sw_dist_sigma`, `sw_exgaussian`, `sw_gamma`, `sw_gaussian`, `sw_gp`, `sw_hurdle_gamma`, `sw_hurdle_lognormal`, `sw_hurdle_pois`, `sw_lognormal`, `sw_ma`, `sw_me`, `sw_mi`, `sw_mixture`, `sw_mono`, `sw_mv_norescor`, `sw_mv_rescor`, `sw_negbinomial`, `sw_nonlinear`, `sw_poisson`, `sw_re_bern`, `sw_re_gauss`, `sw_re_negbin`, `sw_re_pois`, `sw_re_slope`, `sw_se`, `sw_skewnormal`, `sw_spline_s`, `sw_spline_t2`, `sw_sratio`, `sw_student`, `sw_trunc`, `sw_vonmises`, `sw_weibull`, `sw_weights`, `sw_zi_binomial`, `sw_zi_negbin`, `sw_zi_poisson` is from run `927990d5d0dcee9d` (2026-11-14). `i320_gp_expquad`, `i320_gp_matern32`, `i320_mi_nhanes`, `i320_pois_trunc_both`, `i320_pois_trunc_ub`, `i320_sratio_cs`, `i320_sratio_plain` is from run `729e8433e40e69a9` (2026-11-15). `i319_gauss_re`, `i319_negbin_fixed`, `i319_negbin_re`, `i319_pois_fixed`, `i319_pois_re`, `i319_pois_re2` is from run `ada72c9096a10e01` (2026-11-15). `s2_cumulative_cauchit`, `s2_cumulative_cloglog`, `s2_cumulative_probit`, `s2_custom_vint`, `s2_custom_vreal` is from run `68c1ac87a5835bac` (2026-11-15). `rstanarm_glm_gaussian`, `rstanarm_glmer_bernoulli`, `rstanarm_glmer_nb_count`, `rstanarm_polr` is from run `6cb802c3e1828d7c` (2026-11-18). `multinma_ndmm_mspline`, `multinma_smoking_re` is from run `1d3344852145733c` (2026-11-18). `epinow2_estimate_infections`, `epinow2_estimate_secondary` is from run `94ef8fbc3a96310a` (2026-11-18). `mlumr_survival_mspline_relaxed`, `mlumr_survival_relaxed` is from run `961cf306ead263d9` (2026-11-18). `hdbayes_lm_npp`, `hdbayes_pwe_npp` is from run `9cfb9c596b1b7957` (2026-11-18). `ecoensemble_hierarchical_withdrivers`, `ecoensemble_hierarchical_withdrivers_conj` is from run `c71256c7674389d0` (2026-11-18). `beeguts_guts_sd` is from run `43bf029f4b84140f` (2026-11-18). `rbioacc_odetk` is from run `75e50003f2ef32db` (2026-11-18).
<!--/gen-->

<a id="reproduction"></a>

## Appendix: reproduction

| Item | Configuration |
| --- | --- |
| Scope | All <!--gen:benchmark_models-->363<!--/gen--> application models, `--corpus all` |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled |
| Measured source | <!--gen:benchmark_source-->`1e0a66ffbcbed2a66b1aefcb8ea5524356634e5f`<!--/gen-->; tracked differences and executable hashes in manifests |
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

`--corpus` selects a source collection. `--filter TEXT` keeps the models
whose IDs contain `TEXT`, and `--models NAME,...` keeps exactly the named
models. An unknown name is an error.

`tools/gen_benchmark_points.py` reruns the reference runs and merges the
results into the committed points file. By default it reruns every model.
`--models NAME,...` limits it to the named models, and `--missing` limits it
to models with no entry or with changed source or data. `--jobs` and
`--timeout` set the parallelism and the limit for each model. Use
`libstanli.so` on Linux.

The harness reads the points file by default through `--points-file`. It
rejects a model whose source or data no longer matches the file.

CmdStan binaries are built once and reused from a content-addressed cache,
`$STANLI_BENCH_CACHE` or `~/.cache/stanli/corpus-bench`. `--build-cache DIR`
sets another cache directory, and `--no-build-cache` builds inside the run
directory. `--build-jobs N` (default 8) sets how many gradient driver builds
run at once.

The harness rejects a CmdStan checkout that differs from `deps/pins.env` in
its CmdStan, Stan or Stan Math head, its tracked files or its `bin/stanc`.
`--allow-unpinned-cmdstan` turns off this check, and the manifest records
that you used it.

`--stanc` (alias `--cmdstan-stanc`) and `--stancflags` choose CmdStan's
compiler. Stanli uses the `stanli-vectorize-probe` in
`deps/stanc3/embed/<key>/`. `--resume` requires identical sources, inputs,
binaries and settings. When a command times out, the harness kills its
process group. A timeout never counts as a completed measurement.

Each `OUT.tsv` has a sibling directory, `OUT.tsv.run/`. It holds an immutable
manifest (source, input and executable hashes, machine, threads, compilers,
upstream checkouts and build flags). It also holds the frozen inputs, exact
commands, load averages, logs, raw paired timings and per-point scaled
errors. `OUT.tsv` is derived from these records.

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
estimates. Every recorded scaled error must pass the
[numerical check](#numerical-gate). The publisher never fills a missing cell
from an older run, with one exception.

The exception is the loop-vectorization comparison. If CmdStan, its toolchain
and the patched stanc3 are unchanged, you can skip the vectorized sweep. List
the existing runs under `cmdstan-runs/`, and time only the models that they
lack with `--filter MODEL`. A later `--cmdstan-run` takes precedence for the
models that it contains. `publish_corpus_bench.py --vectorized` records in
`combined.json` which run supplied each model. The renderer checks the
inputs, settings, toolchain and compiler hash again.

The [default evidence](../output/corpus-performance/README.md) and
[loop-vectorization evidence](../output/corpus-performance-vectorized/README.md)
directories hold the full inventories, manifests, summary TSVs, raw
timings, command events and checksums. Older runs and protocols are in
[benchmark history](../notes/performance/benchmark-history.md).

<a id="benchmarking-a-change"></a>

## Appendix: benchmarking a change

The corpus run above produces the published results. This appendix covers
how to measure one change against the branch that it targets.

### Compiler-pipeline regression checks

Before you merge a change to the compiler pipeline, benchmark both the
current default branch and the candidate. Run the full inventory
(`--corpus all`, no `--filter` or `--models`) with the shipped configuration
for both revisions, or the [selected models](#selected-models) when the
change qualifies. Use the same hardware, compiler, build mode, threads,
inputs and settings for both, with separate output paths.

Compare each model's source-to-MIR time, preparation time, warm gradient and
total sampling estimate as `100 * (candidate / baseline - 1)`. A model's
slowdown is major when its total sampling estimate grows by 2% or 5 ms,
whichever is greater. Show results for each model and each phase, because a
win in the aggregate can hide a regression. Keep missing, failed or timed-out
observations visible. They cannot count as a pass. Every existing numerical
check still applies.

Shared CI hosts can be noisier than that. If a result crosses the threshold,
investigate with a separate experiment on an idle host. Run at least six
alternating pairs of baseline and candidate, each in a fresh process, for the
affected phases. Keep all samples, and add controls with identical binaries
when needed. Do not rerun until a favorable sample appears. Fix a confirmed
regression before merge. The PR lists both revision SHAs, build identities,
exact commands, coverage, the phase comparison and links to the raw results.

<a id="selected-models"></a>

### Selected models

A change to graph passes, lowering, preparation or kernels (`runtime/`) may
time a selected set of models. Build `bench_grad`, `dump_ops` and
`dump_islands` with the shipped configuration for both revisions, then run:

```sh
python3 tools/bench_select.py deps/posteriordb build/select \
  --base ../main/build-bench --candidate build-bench --opcodes OP_CATEGORICAL
```

The tool reads the same corpus and data as the benchmark. It compiles every
model with each revision's own compiler, prepares it with each revision's
runtime, and selects these models:

- Every model whose prepared program differs: the MIR, the `dump_ops` op list
  or the `dump_islands` output.
- Every model that runs an opcode named with `--opcodes`. Name each opcode
  whose kernel the change edits. A kernel change leaves the program as it
  was, and a tool cannot tell reliably which opcodes a source change touches.
- Every model whose preparation gets slower by more than 2% or 5 ms. The
  tool times preparation for every model in both revisions, one process at a
  time with the revisions alternating, and keeps the best of three. A pass
  can get slower without changing its output.
- Every model that fails to compile, dump or prepare in either revision. It is
  listed as unresolved, and the full corpus run is then required.
- The control models below, whose programs normally do not change.

The controls catch speed changes that come from the binary alone. For
example, PR #600 inserted an opcode in the middle of the opcode list, which
renumbered the opcodes after it. `i319_pois_re` got 3.4% slower with an
identical program. The controls cover different kinds and sizes of model:

| Model | Why it is a control |
| --- | --- |
| `i319_pois_re` | brms Poisson model with random effects; slowed in PR #600 with an identical program |
| `arma11` | short scalar time-series loop; also flagged in PR #600 with an identical program |
| `eight_schools_noncentered` | tiny model, where fixed per-gradient costs dominate |
| `hmm_example` | hidden Markov model with a forward-algorithm loop |
| `one_comp_mm_elim_abs` | ODE solver |
| `gpcm_latent_reg_irt` | 530 parameters and large data |

`OUT/selection.json` holds the evidence: build identities, counts, each
changed model with a one-line summary of its opcode changes, the models that
use the named opcodes, the preparation times of every model, failures, and
the selected models with their reasons. Its `models` field is the list to
pass to the benchmark:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/selected/base.tsv --bench ../main/build-bench/bench_grad \
  --corpus all --models "$(python3 -c 'import json; print(json.load(open("build/select/selection.json"))["models"])')" \
  --rounds 6 --warmup-ms 20 --measure-ms 40 --points 10 \
  --gradient-timeout 60 --build-timeout 900
```

Run the candidate the same way with its own `bench_grad` and output path.
The threshold, re-measurement and evidence rules above apply unchanged. The
PR also reports the selection: the counts, the changed models with their
reasons, the named opcodes, the controls and the preparation comparison over
all models.

The full corpus stays required for compiler pins, source-to-MIR compilation
(`compiler/`, `tools/stanc_embed`, `deps/pins.env`), changes to how the
benchmark harness or timer measures, build flag or toolchain changes, and any run where the
selection tool fails on a model in either revision.

### Focused execution-path benchmarks

The corpus run times gradients only. To time generated quantities,
standalone function calls, and slowdowns when a model falls back to the
interpreter, use the phase benchmarks, which go through the public API:

```sh
# From the repository root, after building the runtime and MIR fixtures.
python3 tools/bench_model_phases.py /absolute/path/libstanli.dylib \
  tools/bench_execution_paths.json ode_nested_for
python3 tools/bench_function_paths.py /absolute/path/libstanli.dylib affine
```

The [execution-path manifest](../tools/bench_execution_paths.json) lists the
workloads: nested loops, runtime indexing, loop and branch callbacks, RNG
output and arguments, runtime integer expressions, solver controls in
generated quantities, bounded temporaries in generated quantities, and canary
models that must stay on the interpreter. `bench_function_paths.py` accepts `affine`,
`branch_exit`, `sized`, `nested_identity`, `nested_scalars` and
`nested_matrices`; the nested cases take an optional outer extent (default 4).

`bench_model_phases.py` reports source compilation, preparation, first and
warm gradients, first and warm output rows, and a seeded 100-warmup/100-draw
fit (`--skip-inference` omits the fit). Its times include Python and ctypes
overhead, so confirm native gradient times with `bench_grad --timed`. Peak
RSS is the `getrusage` memory of the whole process (bytes on macOS, KiB on
Linux).

Collect six alternating pairs of baseline and candidate, in fresh processes
with nothing else running. Keep the raw samples, hashes and build settings,
and check numerical parity separately. These focused runs do not replace the
corpus run or change its published tables.

### Corpus benchmark build cache

The key for a cache entry covers the `.stan` bytes, the stanc binary and
flags, the gradient driver and timer sources, the CmdStan checkout, library,
compiler and environment identity, and the CmdStan path. So worktrees share
entries. An entry holds the model header, the gradient driver, the ordinary
executable and the original stanc and build durations. Stanli compilation,
preparation and every gradient are always measured fresh. The run directory
records the cache location and each model's hit or miss in
`build-prepass.jsonl` and `*.result.json`.
