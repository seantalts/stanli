# Benchmarks

## TL;DR

- For the median of <!--gen:benchmark_models-->363<!--/gen--> application models, Stanli computes
  gradients **<!--gen:corpus_median-->2.16x<!--/gen--> faster** than CmdStan.
- For the median model, [total sampling](#full-corpus) is
  **<!--gen:corpus_total_median-->23x<!--/gen--> faster**, mostly because CmdStan builds C++ code
  for each model. Stanli prepares the structure that depends on the data
  once, then reuses that plan and its buffers
  ([how it works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)).
- Stanli is as fast as CmdStan, or faster, on <!--gen:corpus_at_par-->346<!--/gen--> of the <!--gen:corpus_n_grad-->362<!--/gen--> models
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
fixtures belong to the [numerical corpus](../TESTING.md#every-model) and are not timed
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
| `ch12_m12_4` | 3.6e+03x | 18.6k | 0.0407 | 45.7 | 1.12e+03x |
| `aalto_poisson_hurdle` | 187x | 6.92k | 0.015 | 2.2 | 147x |
| `ch12_m12_3` | 86.8x | 10.8k | 0.0117 | 1.52 | 130x |
| `ch12_m12_3_alt` | 84x | 11k | 0.0117 | 1.52 | 130x |
| `M0_model` | 61.3x | 7.46k | 0.0121 | 1.61 | 133x |
| `Mt_model` | 54.4x | 11k | 0.0144 | 2.53 | 176x |
| `gpcm_latent_reg_irt` | 12.5x | 69k | 8.29 | 107 | 12.9x |
| `grsm_latent_reg_irt` | 11.9x | 63.1k | 4.44 | 56.3 | 12.7x |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.1x | 47.3k | 1.32 | 16.1 | 12.2x |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11.1x | 68.5k | 1.94 | 23.1 | 11.9x |
| `radon_county_intercept` | 10.2x | 62.8k | 2.97 | 31.6 | 10.6x |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.2x | 50.6k | 2.41 | 25.8 | 10.7x |
| <code>radon_variable_slope_</code><br><code>centered</code> | 9.76x | 52.9k | 2.57 | 26.5 | 10.3x |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 9.73x | 81.2k | 3.92 | 40 | 10.2x |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.72x | 102k | 4.99 | 50.2 | 10.1x |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.36x | 77.7k | 5.29 | 50.8 | 9.6x |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.3x | 100k | 6.84 | 65.3 | 9.53x |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.17x | 59.7k | 3.17 | 30.9 | 9.73x |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.08x | 125k | 6.79 | 63.5 | 9.35x |
| `radon_pooled` | 8.25x | 9.33k | 0.414 | 4.82 | 11.7x |
| `s2_custom_vreal` | 8x | 10.2k | 0.0126 | 2.38 | 189x |
| `dogs` | 7.78x | 20.7k | 0.208 | 3.94 | 19x |
| `rats_model` | 7.46x | 54.5k | 0.0499 | 2.2 | 44.1x |
| `arK` | 7.33x | 59.1k | 0.113 | 2.43 | 21.5x |
| `ch16_m16_1` | 6.84x | 301k | 1.26 | 10.4 | 8.24x |
| `s2_multinomial` | 5.53x | 12.9k | 0.0358 | 3.04 | 85x |
| `sw_nonlinear` | 5.43x | 10.9k | 0.0135 | 2.81 | 208x |
| `dogs_log` | 5.18x | 12.1k | 0.124 | 2.61 | 21x |
| `ctsem_ctsm_400` † | 5.17x | 5k | 20.7 | 195 | 9.44x |
| `ctsem_ctsm` | 5.1x | 1.08M | 449 | 2.38e+03 | 5.3x |
| `ch12_m12_7` | 4.97x | 48.6k | 26.8 | 135 | 5.03x |
| `s2_logistic_normal` | 4.94x | 13.6k | 0.071 | 5.69 | 80.1x |
| `ch09_m5_8s` | 4.88x | 1.37M | 0.647 | 4.79 | 7.39x |
| `ch12_m12_5` | 4.77x | 37.4k | 21.8 | 106 | 4.86x |
| `ch09_m5_8s2` | 4.77x | 1.11M | 0.542 | 4.22 | 7.79x |
| `s2_invgaussian` | 4.73x | 16.3k | 0.0154 | 2.64 | 171x |
| `ch14_m14_5` | 4.57x | 12k | 0.0364 | 1.78 | 49x |
| `s2_cumulative_cauchit` | 4.4x | 20.7k | 0.0292 | 2.63 | 90x |
| `logmesquite` | 4.36x | 56.4k | 0.0348 | 2.1 | 60.4x |
| `ch15_m15_1` | 4.36x | 30.7k | 0.0198 | 2.07 | 105x |
| `dogs_hierarchical` | 4.33x | 9.37k | 0.102 | 1.94 | 19.1x |
| `logmesquite_logvas` | 4.29x | 92.3k | 0.0514 | 2.4 | 46.7x |
| `mesquite` | 4.19x | 160k | 0.0846 | 2.23 | 26.4x |
| `s2_nl_noloop` | 4.17x | 52.6k | 0.0339 | 2.94 | 86.8x |
| `ch15_m15_2` | 4.14x | 32.4k | 0.0274 | 2.33 | 85.3x |
| `s2_car_icar` | 4.11x | 1.7M | 0.736 | 6.3 | 8.56x |
| `logmesquite_logva` | 4.09x | 58.7k | 0.0236 | 2.09 | 88.8x |
| `s2_categorical_re` | 4.02x | 34.6k | 0.0794 | 4.66 | 58.7x |
| `election88_full` | 3.99x | 429k | 107 | 430 | 4.02x |
| `nes` | 3.98x | 90.8k | 1.75 | 8.96 | 5.12x |
| `sw_me` | 3.88x | 57.9k | 0.0411 | 3.16 | 76.9x |
| `ch15_m15_9` | 3.88x | 11.3k | 0.0313 | 1.91 | 60.9x |
| `ch15_m15_8` | 3.85x | 10.7k | 0.0299 | 1.74 | 58.3x |
| `logmesquite_logvash` | 3.85x | 93.7k | 0.0494 | 2.31 | 46.7x |
| `sw_dist_sigma` | 3.84x | 10.7k | 0.0157 | 2.75 | 176x |
| `ch14_m14_4x` | 3.83x | 9.05k | 0.0228 | 1.66 | 72.9x |
| `Mth_model` | 3.82x | 33.9k | 0.926 | 6.67 | 7.2x |
| `logmesquite_logvolume` | 3.81x | 12.4k | 0.00927 | 1.82 | 197x |
| `sw_spline_s` | 3.8x | 37.6k | 0.0278 | 3.45 | 124x |
| `ch14_m14_4` | 3.8x | 9.05k | 0.0225 | 1.66 | 73.8x |
| `s2_me2_nomecor` | 3.79x | 50.4k | 0.0567 | 3.49 | 61.6x |
| `multinma_ndmm_mspline` | 3.77x | 35.4k | 0.489 | 28.7 | 58.7x |
| `s2_s_cc` | 3.75x | 30.5k | 0.0213 | 3.25 | 153x |
| `aalto_grp_aov` | 3.69x | 12.5k | 0.00877 | 1.9 | 217x |
| `s2_dist_sigma_re` | 3.67x | 29.4k | 0.0308 | 3.95 | 128x |
| `s2_nlf` | 3.61x | 9.42k | 0.0167 | 2.9 | 173x |
| `sw_weights` | 3.59x | 9.98k | 0.0147 | 2.39 | 163x |
| `ch09_m9_1_chains4` | 3.57x | 11.3k | 0.0155 | 1.92 | 124x |
| `ch09_m9_1` | 3.56x | 11.3k | 0.0155 | 1.92 | 124x |
| `kilpisjarvi` | 3.55x | 1.12M | 0.236 | 2.42 | 10.3x |
| `s2_cox_cens` | 3.55x | 22.1k | 0.0318 | 4.09 | 129x |
| `kidscore_interaction_c` | 3.52x | 15.2k | 0.0491 | 1.99 | 40.6x |
| `kidscore_interaction` | 3.51x | 178k | 0.499 | 3.58 | 7.17x |
| `kidscore_interaction_z` | 3.5x | 24k | 0.0737 | 2.15 | 29.2x |
| `s2_gp_approx` | 3.49x | 34k | 0.0315 | 4.24 | 134x |
| `kidscore_interaction_c2` | 3.49x | 17.7k | 0.0569 | 1.99 | 35x |
| `kidscore_mom_work` | 3.47x | 40.5k | 0.119 | 2.14 | 18x |
| `s2_car_esicar` | 3.47x | 28.9k | 0.0278 | 3.46 | 124x |
| `s2_mm` | 3.42x | 26.6k | 0.0271 | 3.66 | 135x |
| `s2_shifted_lognormal` | 3.42x | 15.1k | 0.0158 | 2.86 | 181x |
| `ch16_m16_4` | 3.41x | 35.3k | 0.065 | 1.91 | 29.4x |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.41x | 17.3k | 0.00973 | 2.11 | 217x |
| `sw_se` | 3.4x | 7.2k | 0.0109 | 2.41 | 220x |
| `s2_index_mi` | 3.4x | 18.6k | 0.0211 | 3.18 | 151x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.4x | 1.03M | 6.62 | 26.2 | 3.96x |
| `s2_mm_weights` | 3.39x | 27.8k | 0.0277 | 3.66 | 132x |
| `logearn_interaction_z` | 3.38x | 22k | 0.188 | 2.56 | 13.7x |
| `s2_s_by` | 3.37x | 70.7k | 0.115 | 5.22 | 45.3x |
| `logearn_interaction` | 3.37x | 322k | 2.65 | 10.7 | 4.06x |
| `sw_lognormal` | 3.35x | 8.28k | 0.0119 | 2.61 | 220x |
| `logearn_height_male` | 3.34x | 189k | 1.11 | 5.44 | 4.88x |
| `logearn_logheight_male` | 3.34x | 715k | 4.2 | 15.8 | 3.75x |
| `s2_car` | 3.32x | 30.6k | 0.031 | 3.36 | 109x |
| `ch15_m15_5` | 3.32x | 21.5k | 0.0173 | 2.09 | 121x |
| `GLMM1_model` | 3.29x | 32.8k | 0.349 | 3.2 | 9.16x |
| `s2_gp_by_approx` | 3.26x | 37.2k | 0.0528 | 5.11 | 96.9x |
| `s2_t2_by` | 3.24x | 36.4k | 0.0711 | 5.01 | 70.4x |
| `radon_county` | 3.24x | 48.8k | 1.32 | 6.19 | 4.67x |
| `sw_spline_t2` | 3.23x | 38.1k | 0.0474 | 3.98 | 83.9x |
| `kidscore_momhsiq` | 3.2x | 90.5k | 0.195 | 2.36 | 12.1x |
| `s2_gr_by` | 3.19x | 33.2k | 0.0275 | 3.93 | 143x |
| `i319_gauss_re` | 3.17x | 41.3k | 0.0724 | 3.69 | 51x |
| `aalto_grp_prior_mean` | 3.13x | 19.3k | 0.0103 | 1.98 | 193x |
| `hdbayes_pwe_npp` | 3.13x | 28.1k | 0.103 | 4.1 | 39.8x |
| `cm_exwald` | 3.05x | 68k | 3.11 | 12.5 | 4.03x |
| `s2_cox` | 3.02x | 30.6k | 0.0353 | 3.13 | 88.8x |
| `s2_hurdle_cumulative` | 3.01x | 11.2k | 0.038 | 3.19 | 84x |
| `sw_re_gauss` | 3.01x | 29.1k | 0.0247 | 3.53 | 143x |
| `ch15_m15_6` | 3.01x | 15.6k | 0.0116 | 1.8 | 155x |
| <code>ecoensemble_</code><br><code>hierarchical_withdrivers</code> | 3x | 163k | 15.7 | 68 | 4.32x |
| `sw_student` | 2.99x | 12.7k | 0.0159 | 2.77 | 174x |
| <code>ecoensemble_</code><br><code>hierarchical_</code><br><code>withdrivers_conj</code> | 2.99x | 192k | 18.6 | 76.4 | 4.1x |
| `aalto_lin` | 2.96x | 18.2k | 0.0107 | 1.92 | 179x |
| `s2_gr_student` | 2.92x | 52.2k | 0.0454 | 4.26 | 93.8x |
| `s2_mi_lognormal` | 2.91x | 14.8k | 0.0205 | 3.56 | 174x |
| `aalto_lin_std` | 2.88x | 11k | 0.00912 | 2.13 | 234x |
| `s2_cumulative_cloglog` | 2.87x | 20.3k | 0.0345 | 2.62 | 76.1x |
| `pilots` | 2.87x | 551k | 0.196 | 2.77 | 14.1x |
| `earn_height` | 2.85x | 150k | 0.594 | 3.29 | 5.55x |
| `bym2_offset_only` | 2.84x | 202k | 8.03 | 25.9 | 3.22x |
| `log10earn_height` | 2.84x | 143k | 0.564 | 3.2 | 5.68x |
| `kidscore_momiq` | 2.84x | 60.1k | 0.0943 | 1.91 | 20.2x |
| `sw_hurdle_lognormal` | 2.83x | 12.5k | 0.0243 | 2.5 | 103x |
| `logearn_height` | 2.83x | 138k | 0.547 | 3.19 | 5.83x |
| `aalto_grp_prior_mean_var` | 2.83x | 59.3k | 0.0244 | 2.84 | 116x |
| `Mtbh_model` | 2.83x | 34.3k | 0.584 | 5.56 | 9.52x |
| `eight_schools_centered` | 2.81x | 32.4k | 0.0111 | 1.8 | 163x |
| `GLMM_Poisson_model` | 2.81x | 75.6k | 0.0558 | 2.83 | 50.6x |
| `blr` | 2.8x | 32.4k | 0.0194 | 2.01 | 103x |
| `sw_acat` | 2.79x | 12.2k | 0.679 | 4.62 | 6.81x |
| `sesame_one_pred_a` | 2.78x | 15.5k | 0.0194 | 1.64 | 84.8x |
| `sw_acat_cs` | 2.76x | 18.3k | 1.21 | 7.21 | 5.96x |
| `s2_mmc` | 2.74x | 35.4k | 0.0616 | 6.16 | 100x |
| `dogs_nonhierarchical` | 2.74x | 56.4k | 0.928 | 8.43 | 9.08x |
| `GLM_Poisson_model` | 2.7x | 29.5k | 0.0199 | 2.16 | 108x |
| `i320_mi_nhanes` | 2.69x | 312k | 0.244 | 3.95 | 16.2x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo2</code> | 2.69x | 34.3k | 0.331 | 12.9 | 38.8x |
| `sw_gamma` | 2.68x | 9.45k | 0.0144 | 2.65 | 184x |
| `ch13_m13_4nc` | 2.68x | 75.5k | 0.7 | 4 | 5.71x |
| `s2_gev` | 2.67x | 12.2k | 0.0357 | 2.75 | 76.8x |
| `kidscore_momhs` | 2.66x | 24.8k | 0.0429 | 1.76 | 41.1x |
| `cm_geg` | 2.64x | 54k | 1.42 | 6.46 | 4.55x |
| `oncobayes2_blrm_exnex_sa` | 2.62x | 25.3k | 0.12 | 12.3 | 103x |
| `hierarchical_gp` | 2.59x | 352k | 6.2 | 24.3 | 3.92x |
| `sw_hurdle_pois` | 2.57x | 10.4k | 0.0275 | 2.29 | 83.3x |
| `accel_gp` | 2.56x | 1.5M | 5.67 | 18.8 | 3.32x |
| `multi_occupancy` | 2.55x | 98.9k | 2.53 | 11.5 | 4.52x |
| `s2_me2` | 2.55x | 71.6k | 0.172 | 6.08 | 35.3x |
| `losscurve_sislob` | 2.55x | 21.3k | 0.0339 | 3.23 | 95.4x |
| `cm_exgaussian` | 2.55x | 14k | 0.23 | 3.08 | 13.4x |
| `accel_splines` | 2.53x | 1.86M | 7.35 | 21.9 | 2.98x |
| `s2_sar` | 2.52x | 10.9k | 0.0499 | 3.09 | 61.9x |
| `ldaK5` † | 2.51x | 5k | 13.1 | 34.4 | 2.62x |
| `s2_sar_error` | 2.47x | 11.4k | 0.0546 | 3.12 | 57.2x |
| `i319_pois_re2` | 2.45x | 173k | 0.835 | 5.77 | 6.91x |
| `seeds_centered_model` | 2.41x | 41.5k | 0.0379 | 2.79 | 73.7x |
| `s2_rate` | 2.39x | 7.8k | 0.0133 | 2.52 | 189x |
| `ch14_m14_3` | 2.39x | 65.2k | 0.621 | 6.36 | 10.2x |
| `ch11_m11_10` | 2.39x | 10.7k | 0.0106 | 2.1 | 198x |
| `ch15_m15_3` | 2.35x | 7.74k | 0.112 | 1.67 | 14.9x |
| `Mh_model` | 2.35x | 53.8k | 0.943 | 4.32 | 4.58x |
| `logistic_regression_rhs` | 2.35x | 137k | 6.32 | 18.6 | 2.94x |
| `sw_re_bern` | 2.35x | 27.4k | 0.0279 | 3.64 | 131x |
| `extra_hurdle_poisson` | 2.34x | 8.4k | 0.00903 | 1.39 | 154x |
| `ch09_m9_5` | 2.32x | 398k | 0.0622 | 1.49 | 23.9x |
| `ch09_m9_2` | 2.3x | 46.7k | 0.00995 | 1.33 | 133x |
| `ch09_m9_4` | 2.3x | 1.55M | 0.224 | 1.85 | 8.26x |
| `ch15_m15_4` | 2.29x | 8.81k | 0.106 | 1.66 | 15.7x |
| `aalto_lin_std_t` | 2.28x | 13k | 0.0109 | 2.37 | 217x |
| `s2_mv_shared_re` | 2.28x | 36.8k | 0.0659 | 6.3 | 95.7x |
| <code>oncobayes2_blrm_exnex_</code><br><code>combo3</code> | 2.26x | 33.5k | 1.11 | 14.5 | 13.1x |
| `sw_skewnormal` | 2.25x | 15.5k | 0.0269 | 2.9 | 108x |
| `ch09_m9_3` | 2.24x | 10.8k | 0.00764 | 1.33 | 173x |
| `sw_vonmises` | 2.22x | 730k | 0.549 | 3.8 | 6.93x |
| `normal_mixture` | 2.21x | 7.96k | 0.324 | 2.07 | 6.37x |
| `ldaK2` | 2.2x | 19.8k | 1.04 | 4.84 | 4.63x |
| `ch11_m11_5` | 2.19x | 29.3k | 0.211 | 2.36 | 11.2x |
| `sw_re_slope` | 2.17x | 53.4k | 0.0708 | 5.99 | 84.6x |
| `ch13_m13_4b` | 2.17x | 34.4k | 0.257 | 2.67 | 10.4x |
| `hier_2pl` | 2.16x | 59.4k | 12.4 | 33 | 2.67x |
| `sw_zi_poisson` | 2.16x | 11.5k | 0.0243 | 2.29 | 94.2x |
| `lsat_model` | 2.16x | 40.8k | 1.89 | 6.65 | 3.52x |
| `ch13_m13_4` | 2.15x | 34.4k | 0.26 | 2.78 | 10.7x |
| `2pl_latent_reg_irt` | 2.15x | 51.2k | 3.83 | 12.6 | 3.28x |
| `dugongs_model` | 2.15x | 33.4k | 0.0242 | 1.88 | 77.7x |
| `ch12_m12_6` | 2.15x | 111k | 149 | 324 | 2.17x |
| `ch13_m13_6` | 2.14x | 36.1k | 0.272 | 2.79 | 10.2x |
| `ch13_m13_7` | 2.14x | 19.9k | 0.00792 | 1.11 | 140x |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.12x | 44.4k | 2.23 | 6.67 | 2.99x |
| `low_dim_gauss_mix` | 2.09x | 14k | 0.743 | 3.57 | 4.81x |
| `sw_re_pois` | 2.07x | 30k | 0.0343 | 3.47 | 101x |
| `prophet` | 2.07x | 1.66M | 63.9 | 136 | 2.13x |
| `cm_rdm` | 2.06x | 67.7k | 8.4 | 20.5 | 2.44x |
| `cm_bisa` | 2.05x | 27.9k | 0.667 | 4.25 | 6.37x |
| `ch09_mp` | 2.03x | 38.9k | 0.00825 | 1.11 | 135x |
| `rstanarm_glm_gaussian` | 2.02x | 14.9k | 0.227 | 24.4 | 108x |
| `sw_cens` | 2.01x | 10.8k | 0.0235 | 3.25 | 138x |
| `irt_2pl` | 2.01x | 61.4k | 1.16 | 5.32 | 4.6x |
| `normal_mixture_k` | 2x | 243k | 49.3 | 101 | 2.05x |
| `cm_lognormal` | 2x | 68k | 2.76 | 8.56 | 3.1x |
| `seeds_model` | 2x | 38.4k | 0.035 | 2.58 | 73.8x |
| `seeds_stanified_model` | 2x | 42.2k | 0.0364 | 2.49 | 68.2x |
| `cm_lnr` | 1.99x | 41.9k | 4.77 | 12.7 | 2.66x |
| `sw_hurdle_gamma` | 1.98x | 13.3k | 0.0356 | 2.5 | 70x |
| `sw_exgaussian` | 1.98x | 25.1k | 0.0322 | 2.85 | 88.4x |
| `ch13_m13_5` | 1.97x | 18.9k | 0.128 | 2.32 | 18.2x |
| `ch11_m11_4` | 1.95x | 16.5k | 0.111 | 2.04 | 18.3x |
| `multinma_smoking_re` | 1.95x | 120k | 0.26 | 17.1 | 65.8x |
| `ch11_m11_8` | 1.94x | 50.9k | 0.0221 | 1.79 | 80.9x |
| `ch13_m13_7nc` | 1.94x | 7.88k | 0.00687 | 1.14 | 166x |
| `ch11_m11_6` | 1.94x | 16.7k | 0.0159 | 1.87 | 117x |
| `soil_incubation` | 1.94x | 203k | 5.02 | 12 | 2.39x |
| `cm_loggamma` | 1.91x | 226k | 6.93 | 16.3 | 2.35x |
| `lotka_volterra` | 1.88x | 75.1k | 2.66 | 8.13 | 3.05x |
| `ch13_m13_1` | 1.87x | 18.5k | 0.0207 | 1.81 | 87.3x |
| `sw_mi` | 1.85x | 15.1k | 0.0193 | 2.78 | 144x |
| `s2_cens_interval` | 1.85x | 8.94k | 0.0253 | 2.72 | 108x |
| `sw_arma` | 1.84x | 42.2k | 0.107 | 3.46 | 32.4x |
| `ch13_m13_2` | 1.84x | 22.9k | 0.0258 | 1.98 | 76.8x |
| `i319_pois_re` | 1.82x | 192k | 0.731 | 4.74 | 6.49x |
| `cm_logweibull` | 1.82x | 90.4k | 1.91 | 6.37 | 3.34x |
| `ch13_m13_3` | 1.82x | 27.8k | 0.0326 | 1.89 | 57.9x |
| `sw_ar` | 1.8x | 10.4k | 0.0272 | 3.2 | 118x |
| `cm_logstudent` | 1.77x | 2.02M | 31.6 | 59.1 | 1.87x |
| `sw_ma` | 1.76x | 11.1k | 0.0297 | 3.21 | 108x |
| `ch11_m11_11` | 1.74x | 143k | 0.0636 | 2.52 | 39.6x |
| `sw_zi_binomial` | 1.73x | 10.2k | 0.0333 | 2.31 | 69.5x |
| `sw_beta` | 1.73x | 8.99k | 0.0256 | 2.82 | 110x |
| `s2_mv_subset` | 1.72x | 12.1k | 0.0155 | 2.36 | 153x |
| `sw_asymlaplace` | 1.72x | 23.4k | 0.0573 | 2.51 | 43.8x |
| `s2_threading` | 1.72x | 11.8k | 0.0127 | 2.26 | 178x |
| `cm_choco` | 1.71x | 15.1k | 0.711 | 4.56 | 6.42x |
| `cm_lnr_bench` | 1.7x | 40.1k | 10.4 | 21 | 2.03x |
| `wells_dist` | 1.68x | 26.5k | 0.689 | 2.9 | 4.21x |
| `cm_gamma` | 1.68x | 2.01M | 43.1 | 75.2 | 1.75x |
| `cm_weibull` | 1.68x | 2.03M | 38.7 | 67.7 | 1.75x |
| `s2_zoi_beta` | 1.66x | 12.1k | 0.0606 | 2.64 | 43.6x |
| `s2_zi_beta` | 1.65x | 10.2k | 0.0516 | 2.59 | 50.1x |
| `cm_lba1` | 1.64x | 145k | 8.28 | 16.8 | 2.03x |
| `sw_mv_norescor` | 1.63x | 11.6k | 0.0163 | 2.36 | 145x |
| `GLM_Binomial_model` | 1.62x | 13.3k | 0.02 | 2.14 | 107x |
| `sw_gaussian` | 1.62x | 10.9k | 0.0116 | 2.17 | 188x |
| `cm_betagate` | 1.61x | 11.8k | 0.435 | 3.53 | 8.13x |
| `s2_mixture_theta` | 1.61x | 80.4k | 0.46 | 4.17 | 9.06x |
| `surgical_model` | 1.6x | 22.2k | 0.0169 | 2.29 | 135x |
| `sir` | 1.58x | 127k | 21.1 | 36.4 | 1.72x |
| `ch11_m11_7` | 1.58x | 7.03k | 0.00854 | 1.67 | 195x |
| `hmm_example` | 1.58x | 25.1k | 0.478 | 3.7 | 7.74x |
| `cm_invweibull` | 1.57x | 87.1k | 2.09 | 6.18 | 2.96x |
| `s2_unstr` | 1.52x | 46.4k | 0.226 | 6.92 | 30.6x |
| `cm_invgamma` | 1.52x | 79k | 1.88 | 5.75 | 3.06x |
| `gp_pois_regr` | 1.52x | 396k | 0.787 | 5.9 | 7.49x |
| `Mb_model` | 1.52x | 11k | 0.427 | 2.71 | 6.34x |
| `cm_lba2` | 1.51x | 140k | 13.8 | 24.5 | 1.77x |
| `s2_com_poisson` | 1.51x | 42.5k | 0.869 | 3.81 | 4.38x |
| `s2_frechet` | 1.51x | 33.2k | 0.0769 | 2.96 | 38.5x |
| `Rate_4_model` | 1.49x | 8.3k | 0.00815 | 1.31 | 161x |
| `sw_re_negbin` | 1.48x | 30.3k | 0.0684 | 3.77 | 55.2x |
| `gp_regr` | 1.48x | 8.82k | 0.0288 | 4.47 | 155x |
| `sw_weibull` | 1.48x | 10.3k | 0.0317 | 2.81 | 88.8x |
| `aalto_bern` | 1.47x | 5.11k | 0.00752 | 1.35 | 179x |
| `sw_zi_negbin` | 1.47x | 20.5k | 0.0742 | 2.46 | 33.2x |
| `s2_gp_by_gr` | 1.45x | 59.6k | 0.446 | 8.79 | 19.7x |
| `ch14_m14_6` | 1.45x | 21.6k | 1.71 | 8.16 | 4.76x |
| `ch14_m14_6x` | 1.44x | 21.6k | 1.72 | 8.16 | 4.75x |
| `covid19imperial_v2` | 1.44x | 473k | 123 | 183 | 1.49x |
| `kronecker_gp` | 1.43x | 2M | 335 | 488 | 1.46x |
| `covid19imperial_v3` | 1.43x | 473k | 124 | 183 | 1.48x |
| `aalto_binom` | 1.43x | 5.11k | 0.00709 | 1.19 | 168x |
| `s2_zi_asymlaplace` | 1.42x | 28.3k | 0.0991 | 2.62 | 26.5x |
| `ch14_m14_2` | 1.42x | 135k | 2.34 | 9.45 | 4.04x |
| `i320_gp_expquad` | 1.42x | 52.4k | 0.59 | 6.92 | 11.7x |
| `Rate_1_model` | 1.41x | 4.69k | 0.00715 | 1.19 | 166x |
| `ch15_m15_7` | 1.41x | 28k | 0.271 | 6.6 | 24.4x |
| `rstanarm_glmer_nb_count` | 1.4x | 221k | 1.79 | 18.3 | 10.3x |
| `ch14_m14_1` | 1.39x | 37.6k | 0.201 | 6.25 | 31.2x |
| `aalto_binom2` | 1.37x | 7.7k | 0.00878 | 1.31 | 149x |
| `sw_mono` | 1.35x | 20.4k | 0.0419 | 2.94 | 70.1x |
| `ch14_m14_8nc` | 1.34x | 263k | 0.763 | 5.68 | 7.44x |
| `sw_binomial` | 1.34x | 7.52k | 0.0227 | 2.46 | 108x |
| `ch11_m_pois` | 1.34x | 6.85k | 0.00779 | 1.32 | 169x |
| `sw_mv_rescor` | 1.33x | 13.3k | 0.084 | 5.34 | 63.7x |
| `Rate_2_model` | 1.33x | 7.31k | 0.00847 | 1.34 | 158x |
| `Rate_5_model` | 1.33x | 5.33k | 0.00832 | 1.3 | 157x |
| `i320_sratio_cs` | 1.33x | 20.4k | 1.65 | 6.1 | 3.71x |
| `ch12_m12_2` | 1.32x | 64.2k | 0.0662 | 2.93 | 44.3x |
| `aalto_binomb` | 1.32x | 5.61k | 0.00757 | 1.21 | 160x |
| `sw_cratio_cs` | 1.32x | 13.7k | 1.12 | 5.11 | 4.56x |
| `i319_negbin_re` | 1.32x | 66.7k | 0.684 | 4.57 | 6.68x |
| `sw_mixture` | 1.32x | 303k | 1.62 | 5.83 | 3.6x |
| `ch12_m12_1` | 1.3x | 11.8k | 0.0214 | 2.06 | 96.5x |
| `ch11_m11_9` | 1.3x | 5.08k | 0.00793 | 1.4 | 176x |
| `Rate_3_model` | 1.29x | 5.2k | 0.00753 | 1.2 | 159x |
| `sw_gp` | 1.29x | 47.3k | 0.948 | 7.33 | 7.73x |
| `s2_beta_binomial` | 1.28x | 10.4k | 0.0544 | 2.65 | 48.8x |
| <code>epinow2_estimate_</code><br><code>infections</code> | 1.28x | 87.8k | 3.53 | 18 | 5.11x |
| `s2_mo_simo_prior` | 1.27x | 21.3k | 0.0461 | 2.93 | 63.5x |
| `nn_rbm1bJ10` | 1.26x | 2.03M | 332 | 424 | 1.28x |
| <code>epinow2_estimate_</code><br><code>secondary</code> | 1.25x | 42.6k | 0.745 | 9.79 | 13.1x |
| `sw_sratio` | 1.23x | 10.4k | 0.65 | 3.27 | 5.04x |
| `ch14_m14_9` | 1.23x | 12.5k | 4.54 | 8.56 | 1.88x |
| `ch14_m14_10` | 1.23x | 11.2k | 4.38 | 8.32 | 1.9x |
| `i320_sratio_plain` | 1.22x | 15.4k | 0.967 | 3.81 | 3.95x |
| `sw_cratio` | 1.22x | 10.4k | 0.646 | 3.26 | 5.05x |
| `s2_cumulative_probit` | 1.21x | 13.7k | 0.0691 | 2.66 | 38.5x |
| `s2_custom_vint` | 1.2x | 10.5k | 0.0629 | 2.47 | 39.3x |
| `s2_dirichlet` | 1.2x | 12.6k | 0.154 | 3.47 | 22.5x |
| `sw_bernoulli` | 1.2x | 8.63k | 0.012 | 2.13 | 177x |
| `sw_cumulative_cs` | 1.19x | 22.3k | 1.22 | 5.14 | 4.22x |
| `ch14_m14_8` | 1.19x | 204k | 0.607 | 4.14 | 6.81x |
| `ch14_m14_11` | 1.18x | 12.7k | 7.68 | 11.9 | 1.55x |
| `s2_discrete_weibull` | 1.17x | 31.8k | 0.12 | 2.48 | 20.7x |
| `ch14_m14_7` | 1.16x | 143k | 4.29 | 12.9 | 3.02x |
| `sw_poisson` | 1.16x | 9.85k | 0.0142 | 1.97 | 139x |
| `Survey_model` | 1.13x | 11.1k | 0.673 | 2.54 | 3.78x |
| `bones_model` | 1.13x | 14.7k | 0.739 | 3.15 | 4.27x |
| `sw_categorical` | 1.11x | 11k | 0.0229 | 3.26 | 142x |
| `nn_rbm1bJ100` † | 1.09x | 5k | 2.24e+03 | 2.44e+03 | 1.09x |
| `hmm_gaussian` | 1.09x | 63.9k | 17.5 | 22.6 | 1.29x |
| `garch11` | 1.08x | 23.3k | 0.199 | 1.97 | 9.92x |
| `s2_hurdle_negbin` | 1.08x | 15.2k | 0.113 | 2.48 | 21.9x |
| `sw_negbinomial` | 1.07x | 1.49M | 2.15 | 4.55 | 2.12x |
| `sw_trunc` | 1.07x | 27.2k | 0.186 | 2.65 | 14.2x |
| `s2_weights_trunc` | 1.06x | 25.1k | 0.178 | 2.71 | 15.2x |
| `hmm_drive_1` | 1.06x | 39.6k | 6.04 | 10.1 | 1.67x |
| `arma11` | 1.05x | 11.2k | 0.0716 | 1.85 | 25.9x |
| `rstanarm_polr` | 1.05x | 15.2k | 0.215 | 5.13 | 23.8x |
| `i319_pois_fixed` | 1.04x | 24k | 0.0762 | 2.03 | 26.6x |
| `wells_dist100_model` | 1.03x | 11.5k | 0.215 | 2.27 | 10.6x |
| `iohmm_reg` | 1.03x | 462k | 165 | 176 | 1.07x |
| `wells_interaction_model` | 1.03x | 32.1k | 0.69 | 2.82 | 4.08x |
| `nes_logit_model` | 1.03x | 17.8k | 0.132 | 2.11 | 16x |
| `wells_dae_c_model` | 1.03x | 16.5k | 0.338 | 2.57 | 7.62x |
| `aalto_poisson_simple` | 1.02x | 5.02k | 0.013 | 1.45 | 112x |
| `s2_ar_cov` | 1.02x | 11.2k | 0.0881 | 6.32 | 71.7x |
| `wells_dae_model` | 1.02x | 24.2k | 0.522 | 2.61 | 4.99x |
| `wells_dae_inter_model` | 1.02x | 12.8k | 0.294 | 2.51 | 8.53x |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.02x | 11.2k | 0.246 | 2.41 | 9.82x |
| `wells_daae_c_model` | 1.02x | 17.6k | 0.395 | 2.65 | 6.7x |
| `i319_negbin_fixed` | 1.02x | 19.4k | 0.193 | 2.44 | 12.6x |
| `mlumr_survival_relaxed` | 1.01x | 96.9k | 13.3 | 19.3 | 1.45x |
| `s2_wiener` | 1.01x | 14.7k | 0.376 | 2.91 | 7.74x |
| `diamonds` | 1.01x | 1.51M | 51.4 | 54.3 | 1.06x |
| `s2_mi_trunc_lb` | 1.01x | 37.9k | 0.217 | 3.61 | 16.6x |
| `wells_dist100ars_model` | 1x | 19.1k | 0.38 | 2.44 | 6.44x |
| `cm_betadiscrete` | 1x | 9.91k | 23.9 | 27 | 1.13x |
| `i320_pois_trunc_ub` | 0.999x | 21.9k | 0.6 | 2.96 | 4.93x |
| `one_comp_mm_elim_abs` | 0.988x | 18.9k | 9.1 | 11.4 | 1.25x |
| `sw_cumulative` | 0.982x | 9.89k | 0.236 | 2.89 | 12.2x |
| `s2_cosy` | 0.976x | 14.7k | 0.105 | 5.6 | 53.4x |
| `cm_ddm` † | 0.969x | 5k | 877 | 857 | 0.977x |
| `i320_pois_trunc_both` | 0.968x | 21.9k | 0.757 | 3.16 | 4.17x |
| `hdbayes_lm_npp` | 0.96x | 88.7k | 0.237 | 6.8 | 28.7x |
| `s2_fcor` | 0.949x | 9.87k | 0.153 | 4.02 | 26.2x |
| `hmm_drive_0` | 0.944x | 24.5k | 3.84 | 7.33 | 1.91x |
| `rbioacc_odetk` | 0.933x | 20.4k | 5.71 | 10.9 | 1.91x |
| `i320_gp_matern32` | 0.925x | 133k | 4.35 | 10 | 2.31x |
| <code>mlumr_survival_mspline_</code><br><code>relaxed</code> | 0.903x | 63.3k | 7.88 | 14.2 | 1.81x |
| `rstanarm_glmer_bernoulli` | 0.873x | 57.3k | 1.29 | 16 | 12.4x |
| `beeguts_guts_sd` | 0.828x | 18.9k | 23.6 | 23.8 | 1.01x |
| `cm_invgaussian` † | 0.769x | 5k | 0.796 | 4.15 | 5.22x |
| `aalto_gpareto` | 0.765x | 9.37k | 0.0168 | 1.7 | 101x |
<!--/gen-->

<!--gen:benchmark_table_notes-->
Run `c08b96efbdcf4d78` (2026-11-19).

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
With loop vectorization on, the median model's gradient is 1.90x faster in Stanli than in CmdStan, against 2.16x for default CmdStan. The speedup changes by more than 10% on 163 of 361 models: it shrinks on 155 and grows on 8. These 10 models change most, in how many times faster Stanli's gradient is:

| Model | Default CmdStan | Vectorized CmdStan |
| --- | ---: | ---: |
| `radon_pooled` | 8.25x | 1.98x |
| `sw_nonlinear` | 5.43x | 2.14x |
| `dogs` | 7.78x | 3.26x |
| `blr` | 2.8x | 1.28x |
| `ch14_m14_5` | 4.57x | 2.13x |
| `s2_nl_noloop` | 4.17x | 1.95x |
| `election88_full` | 3.99x | 1.89x |
| `logistic_regression_rhs` | 2.35x | 1.13x |
| `sw_dist_sigma` | 3.84x | 1.9x |
| `ch14_m14_4x` | 3.83x | 1.95x |
<!--/gen-->

## Fast mode

<!--gen:fast_lead-->
The fast mode run is from 2026-11-13 (Stanli source `b4247512`), with models
compiled and run in fast mode (`corpus_bench.py --fast-math`). The default
mode run is from 2026-11-19 (Stanli source `29602320`). The two runs used
different binaries, so the comparison below is between runs.
<!--/gen-->

| Fast mode | Result |
| --- | ---: |
| Models with paired gradient timings | <!--gen:fast_population-->346<!--/gen--> |
| Median speedup over default mode | <!--gen:fast_median-->1.06x<!--/gen--> |
| Geometric-mean speedup over default mode | <!--gen:fast_geomean-->1.83x<!--/gen--> |
| Models at least 2x faster than default mode | <!--gen:fast_ge_two-->77<!--/gen--> |
| Models at least 10x faster | <!--gen:fast_ge_ten-->33<!--/gen--> |
| Models at least 100x faster | <!--gen:fast_ge_hundred-->10<!--/gen--> |
| Models faster than CmdStan in fast mode | <!--gen:fast_beats_fast-->338<!--/gen--> |
| Models faster than CmdStan in default mode | <!--gen:fast_beats_default-->335<!--/gen--> |

The large gains come from models whose likelihood repeats over the data. Fast
mode evaluates such a term once per distinct row or group.
In fast mode, <!--gen:fast_slowest-->the slowest model, `s2_multinomial`, is 4.90x slower.<!--/gen-->

The largest gains, gradient time in microseconds:

<!--gen:fast_top_table-->
| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 43 | 0.0754 | 571x faster |
| `nes` | 19.2 | 0.0727 | 264x faster |
| `logearn_interaction_z` | 8.18 | 0.0509 | 161x faster |
| `logearn_interaction` | 8.19 | 0.0511 | 160x faster |
| `logearn_logheight_male` | 5.87 | 0.0446 | 131x faster |
| `logearn_height_male` | 5.84 | 0.0448 | 130x faster |
| `diamonds` | 34.1 | 0.306 | 111x faster |
| `earn_height` | 3.9 | 0.0382 | 102x faster |
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

Each entry also records the Stanli build id and C ABI version of the library
that ran its reference run. These two values are provenance and nothing
compares them with the build under test. The file mixes builds on purpose:
`tools/gen_benchmark_points.py --missing` runs only new or changed models with
the current build and keeps every other entry. The ABI version counts changes
to the C function signatures, and the points are plain lists of doubles that
every build reads the same way, so an entry stays usable after the ABI number
moves. What can make an entry wrong is a changed model source or data, which
the loader rejects through the two hashes, or a changed number of parameters,
which the timing drivers reject. A newer sampler would draw different points
and take a somewhat different number of gradients. That moves the total
sampling estimate for the model. It does not move the gradient comparison,
because both engines evaluate the same points. Rerun a model's reference run
with `--models` when a change to NUTS or adaptation should move its count.

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
CmdStan stanc and build times for 70 of 363 models were reused from the build cache and not remeasured. 61 models were measured while the 1-minute load average was above 6 and timed again. `s2_me2`, `s2_me2_nomecor`, `s2_mi_lognormal`, `s2_mi_trunc_lb`, `s2_mixture_theta`, `s2_mm`, `s2_mm_weights`, `s2_mmc`, `s2_mo_simo_prior`, `s2_multinomial`, `s2_mv_shared_re`, `s2_mv_subset`, `s2_nl_noloop`, `s2_nlf`, `s2_rate`, `s2_s_by`, `s2_s_cc`, `s2_sar`, `seeds_stanified_model`, `sesame_one_pred_a`, `sir`, `soil_incubation`, `state_space_stochastic_level_stochastic_seasonal`, `surgical_model`, `sw_acat`, `sw_acat_cs`, `sw_ar`, `sw_arma`, `sw_asymlaplace`, `sw_bernoulli`, `sw_beta`, `sw_binomial`, `sw_categorical`, `sw_cens`, `sw_cratio`, `sw_cratio_cs`, `sw_cumulative`, `sw_cumulative_cs`, `sw_dist_sigma`, `sw_exgaussian`, `sw_gamma`, `sw_gaussian`, `sw_gp`, `sw_hurdle_gamma`, `sw_hurdle_lognormal`, `sw_hurdle_pois`, `sw_lognormal`, `sw_ma`, `sw_me`, `sw_mi`, `sw_mixture`, `sw_mono`, `sw_mv_norescor`, `sw_mv_rescor`, `sw_negbinomial`, `sw_nonlinear`, `sw_poisson`, `sw_re_bern`, `sw_re_gauss`, `sw_re_negbin`, `sw_re_pois` are from run `881dbca09738e257` (2026-11-20).

Stanli timings in the loop-vectorization comparison are from the default run. The CmdStan timings with loop vectorization are reused from run `937801ca404cae01` (2026-11-13). `sw_acat`, `sw_acat_cs`, `sw_ar`, `sw_arma`, `sw_asymlaplace`, `sw_bernoulli`, `sw_beta`, `sw_binomial`, `sw_categorical`, `sw_cens`, `sw_cratio`, `sw_cratio_cs`, `sw_cumulative`, `sw_cumulative_cs`, `sw_dist_sigma`, `sw_exgaussian`, `sw_gamma`, `sw_gaussian`, `sw_gp`, `sw_hurdle_gamma`, `sw_hurdle_lognormal`, `sw_hurdle_pois`, `sw_lognormal`, `sw_ma`, `sw_me`, `sw_mi`, `sw_mixture`, `sw_mono`, `sw_mv_norescor`, `sw_mv_rescor`, `sw_negbinomial`, `sw_nonlinear`, `sw_poisson`, `sw_re_bern`, `sw_re_gauss`, `sw_re_negbin`, `sw_re_pois`, `sw_re_slope`, `sw_se`, `sw_skewnormal`, `sw_spline_s`, `sw_spline_t2`, `sw_sratio`, `sw_student`, `sw_trunc`, `sw_vonmises`, `sw_weibull`, `sw_weights`, `sw_zi_binomial`, `sw_zi_negbin`, `sw_zi_poisson` is from run `927990d5d0dcee9d` (2026-11-14). `i320_gp_expquad`, `i320_gp_matern32`, `i320_mi_nhanes`, `i320_pois_trunc_both`, `i320_pois_trunc_ub`, `i320_sratio_cs`, `i320_sratio_plain` is from run `729e8433e40e69a9` (2026-11-15). `i319_gauss_re`, `i319_negbin_fixed`, `i319_negbin_re`, `i319_pois_fixed`, `i319_pois_re`, `i319_pois_re2` is from run `ada72c9096a10e01` (2026-11-15). `s2_cumulative_cauchit`, `s2_cumulative_cloglog`, `s2_cumulative_probit`, `s2_custom_vint`, `s2_custom_vreal` is from run `68c1ac87a5835bac` (2026-11-15). `rstanarm_glm_gaussian`, `rstanarm_glmer_bernoulli`, `rstanarm_glmer_nb_count`, `rstanarm_polr` is from run `6cb802c3e1828d7c` (2026-11-18). `multinma_ndmm_mspline`, `multinma_smoking_re` is from run `1d3344852145733c` (2026-11-18). `epinow2_estimate_infections`, `epinow2_estimate_secondary` is from run `94ef8fbc3a96310a` (2026-11-18). `mlumr_survival_mspline_relaxed`, `mlumr_survival_relaxed` is from run `961cf306ead263d9` (2026-11-18). `hdbayes_lm_npp`, `hdbayes_pwe_npp` is from run `9cfb9c596b1b7957` (2026-11-18). `ecoensemble_hierarchical_withdrivers`, `ecoensemble_hierarchical_withdrivers_conj` is from run `c71256c7674389d0` (2026-11-18). `beeguts_guts_sd` is from run `43bf029f4b84140f` (2026-11-18). `rbioacc_odetk` is from run `75e50003f2ef32db` (2026-11-18).
<!--/gen-->

<a id="reproduction"></a>

## Appendix: reproduction

| Item | Configuration |
| --- | --- |
| Scope | All <!--gen:benchmark_models-->363<!--/gen--> application models, `--corpus all` |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled |
| Measured source | <!--gen:benchmark_source-->`29602320eb084eed320faa10216a3f915c163440`<!--/gen-->; tracked differences and executable hashes in manifests |
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

A default run can also replace the models that were timed while the machine
was busy. The recorded load alone decides which models are replaced.
`tools/loaded_models.py` lists the models whose 1-minute load
average went above the threshold (default 6) at the start of any Stanli
compile, gradient or preparation command. Time exactly those models again
from the same checkout, binaries and settings as the first run, then publish
both runs:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/rerun.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 20 --measure-ms 40 --points 10 \
  --gradient-timeout 60 --build-timeout 900 \
  --models "$(python3 tools/loaded_models.py build/corpus-current/default.tsv)"
python3 tools/publish_corpus_bench.py build/corpus-current/default.tsv \
  output/corpus-performance --rerun build/corpus-current/rerun.tsv --max-load 6
```

The publisher rejects a rerun whose manifest identity differs from the first
run's in anything other than its model list. The first rerun must contain
exactly the loaded models, and its results are used even when they are
slower or failed. A model whose rerun was loaded again is noted in the
report. A further `--rerun` may time only those models. `substitutions.json`
records the threshold and which run supplied each model, and each rerun is
kept under `reruns/RUN_ID/`. The renderer checks the load rule again from
the published events. Run each rerun once and publish what it measured.

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

A change to graph passes, lowering, preparation or kernels (`runtime/`), or to
the stanc3 pin (`STANC3_SRC_SHA` in `deps/pins.env`), may time a selected set
of models. Build `bench_grad`, `dump_ops` and
`dump_islands` with the shipped configuration for both revisions, then run:

```sh
python3 tools/bench_select.py deps/posteriordb build/select \
  --base ../main/build-bench --candidate build-bench --opcodes OP_CATEGORICAL
```

The tool reads the same corpus and data as the benchmark. It compiles every
model with each revision's own compiler, prepares it with each revision's
runtime, and selects these models:

- Every model whose prepared program differs: the MIR, the `dump_ops` op list
  of the log-density graph or of the `write_array` graph (`dump_ops
  --write-array`; each list includes the ops inside each retained loop body),
  or the `dump_islands` output. Some models keep their retained loops only in
  the `write_array` graph, for example `covid19imperial_v2`,
  `covid19imperial_v3` and `iohmm_reg`.
- Every model that runs an opcode named with `--opcodes` in either graph or
  an island. Name each opcode
  whose kernel the change edits. A kernel change leaves the program as it
  was, and a tool cannot tell reliably which opcodes a source change touches.
  Opcodes match by exact name. A retained loop runs some ops under their
  `_DYNAMIC` names, for example `MATRIX_EXP_DYNAMIC` in the ctsem models, so
  name those as well.
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
identical program. The controls cover different kinds and sizes of model.
The last column is the spread of the paired gradient speedup in the
[published run](../output/corpus-performance/README.md): the median absolute
deviation of the paired speedup, as a fraction of the speedup:

| Model | Why it is a control | Spread |
| --- | --- | ---: |
| `i319_pois_re` | brms Poisson model with random effects; slowed in PR #600 with an identical program | 0.70% |
| `sw_ar` | brms autoregressive model; a short loop in which each step reads the previous residuals | 0.46% |
| `eight_schools_noncentered` | tiny model, where fixed per-gradient costs dominate | 0.51% |
| `hmm_example` | hidden Markov model with a forward-algorithm loop | 0.93% |
| `one_comp_mm_elim_abs` | ODE solver | 1.46% |
| `gpcm_latent_reg_irt` | 530 parameters and large data | 0.41% |
| `cm_lba2` | cogmod model that runs as a region map | 0.20% |
| `mlumr_survival_mspline_relaxed` | mlumr model with two retained loops (`OP_LOOP`) | 0.24% |
| `hierarchical_gp` | Gaussian process covariance with Cholesky factors, 933 parameters | 0.04% |
| `kronecker_gp` | dense linear algebra: symmetric eigendecomposition and matrix products | 0.41% |
| `oncobayes2_blrm_exnex_combo3` | oncobayes2 model: CRAN package code with `multi_normal_cholesky` | 0.31% |

A control is only useful if its own timing is steadier than the shift it has
to expose. The first version of this table used `arma11`, a short scalar
time-series loop. It has the largest spread of the 362 timed models, 7.6%
against a median of 0.62%, so it could not show a 2% to 4% change. `sw_ar`
replaces it with a model of the same kind and a spread of 0.46%. The new rows
fill the paths that no earlier control reached: the region map, retained
loops, Gaussian processes, dense linear algebra and a CRAN package. Each new
control has a spread below 0.5%, and `tests/test_bench_select.py` fails if
any control's spread in the published run exceeds 1.5%.

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

A stanc3 pin change can use the selection because the tool compiles each
model with the compiler that each revision's pin names. A model whose MIR or
prepared program is the same under both pins runs the same code, so timing it
again measures only noise. Build the embedded compiler for both pins before
the selection (`tools/stanc_embed/build.sh`), and report both pins in the PR.

The full corpus stays required for other changes to source-to-MIR compilation
(`compiler/`, `tools/stanc_embed`, the other pins in `deps/pins.env`), changes
to how the benchmark harness or timer measures, build flag or toolchain
changes, and any run where the selection tool fails on a model in either
revision.

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
