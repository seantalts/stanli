# Corpus status

Evaluating: 119/120
Differentially verified against CmdStan: 118/120

A model counts as passing only when tools/verify_sample.py matches CmdStan's log_prob and full gradient at the shared deterministic point. Accuracy below is the worst deviation over lp and every gradient component: relative, and in ULPs (0 = bitwise identical to CmdStan). Bitwise counts are reported for information; the numeric policy gate is the ULP budget. Models that evaluate but are not verified are listed separately and are not counted.

| model | values compared | max rel diff | max ULP |
| --- | ---: | ---: | ---: |
| `2pl_latent_reg_irt` | 532 | 8.8e-16 | 64 |
| `GLMM1_model` | 238 | 1.5e-14 | 104 |
| `GLMM_Poisson_model` | 46 | 1.8e-15 | 16 |
| `GLM_Binomial_model` | 4 | 0 (bitwise) | 0 |
| `GLM_Poisson_model` | 5 | 2.2e-16 | 1 |
| `M0_model` | 3 | 1.1e-14 | 62 |
| `Mb_model` | 4 | 4.9e-14 | 332 |
| `Mh_model` | 389 | 5.5e-15 | 42 |
| `Mt_model` | 5 | 1.8e-14 | 106 |
| `Mtbh_model` | 155 | 9.7e-16 | 23 |
| `Mth_model` | 395 | 3.4e-15 | 96 |
| `Rate_1_model` | 2 | 0 (bitwise) | 0 |
| `Rate_2_model` | 3 | 0 (bitwise) | 0 |
| `Rate_3_model` | 2 | 0 (bitwise) | 0 |
| `Rate_4_model` | 3 | 1.3e-16 | 1 |
| `Rate_5_model` | 2 | 0 (bitwise) | 0 |
| `Survey_model` | 2 | 7.5e-15 | 54 |
| `accel_gp` | 67 | 3.4e-16 | 2 |
| `accel_splines` | 83 | 3.1e-16 | 2 |
| `arK` | 8 | 9.6e-16 | 5 |
| `arma11` | 5 | 0 (bitwise) | 0 |
| `blr` | 7 | 1.8e-16 | 1 |
| `bones_model` | 14 | 1.5e-16 | 1 |
| `bym2_offset_only` | 3846 | 2.0e-15 | 18 |
| `covid19imperial_v2` | 52 | 8.2e-16 | 7 |
| `covid19imperial_v3` | 52 | 8.2e-16 | 7 |
| `diamonds` | 27 | 2.6e-12 | 16248 |
| `dogs` | 4 | 5.5e-15 | 31 |
| `dogs_hierarchical` | 3 | 1.2e-15 | 9 |
| `dogs_log` | 3 | 0 (bitwise) | 0 |
| `dogs_nonhierarchical` | 66 | 6.9e-16 | 4 |
| `dugongs_model` | 5 | 1.9e-16 | 1 |
| `earn_height` | 4 | 0 (bitwise) | 0 |
| `eight_schools_centered` | 11 | 0 (bitwise) | 0 |
| `eight_schools_noncentered` | 11 | 0 (bitwise) | 0 |
| `election88_full` | 91 | 9.8e-15 | 81 |
| `garch11` | 5 | 1.6e-15 | 8 |
| `gp_pois_regr` | 14 | 3.9e-16 | 2 |
| `gp_regr` | 4 | 1.2e-16 | 1 |
| `gpcm_latent_reg_irt` | 531 | 2.6e-13 | 4608 |
| `grsm_latent_reg_irt` | 409 | 1.2e-14 | 81 |
| `hier_2pl` | 670 | 0 (bitwise) | 0 |
| `hierarchical_gp` | 934 | 8.9e-16 | 88 |
| `hmm_drive_0` | 7 | 6.2e-16 | 3 |
| `hmm_drive_1` | 7 | 1.0e-15 | 8 |
| `hmm_example` | 5 | 4.6e-16 | 4 |
| `hmm_gaussian` | 15 | 7.0e-15 | 36 |
| `iohmm_reg` | 30 | 2.3e-14 | 2768 |
| `irt_2pl` | 145 | 0 (bitwise) | 0 |
| `kidscore_interaction` | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_c` | 6 | 4.7e-14 | 330 |
| `kidscore_interaction_c2` | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_z` | 6 | 5.6e-14 | 363 |
| `kidscore_mom_work` | 6 | 0 (bitwise) | 0 |
| `kidscore_momhs` | 4 | 0 (bitwise) | 0 |
| `kidscore_momhsiq` | 5 | 1.4e-16 | 1 |
| `kidscore_momiq` | 4 | 0 (bitwise) | 0 |
| `kilpisjarvi` | 4 | 0 (bitwise) | 0 |
| `ldaK2` | 8 | 6.8e-14 | 452 |
| `ldaK5` | 7715 | 7.1e-13 | 10698848 |
| `log10earn_height` | 4 | 0 (bitwise) | 0 |
| `logearn_height` | 4 | 4.3e-16 | 2 |
| `logearn_height_male` | 5 | 0 (bitwise) | 0 |
| `logearn_interaction` | 6 | 2.6e-16 | 2 |
| `logearn_interaction_z` | 6 | 1.1e-15 | 9 |
| `logearn_logheight_male` | 5 | 2.1e-16 | 1 |
| `logistic_regression_rhs` | 3076 | 2.6e-15 | 2048 |
| `logmesquite` | 9 | 3.9e-16 | 3 |
| `logmesquite_logva` | 6 | 1.3e-16 | 1 |
| `logmesquite_logvas` | 9 | 2.0e-16 | 1 |
| `logmesquite_logvash` | 8 | 3.4e-16 | 2 |
| `logmesquite_logvolume` | 4 | 1.7e-16 | 1 |
| `losscurve_sislob` | 16 | 4.8e-16 | 4 |
| `lotka_volterra` | 9 | 4.3e-15 | 22 |
| `low_dim_gauss_mix` | 6 | 0 (bitwise) | 0 |
| `low_dim_gauss_mix_collapse` | 6 | 0 (bitwise) | 0 |
| `lsat_model` | 1007 | 2.7e-15 | 96 |
| `mesquite` | 9 | 0 (bitwise) | 0 |
| `multi_occupancy` | 107 | 4.8e-15 | 36 |
| `nes` | 11 | 0 (bitwise) | 0 |
| `nes_logit_model` | 3 | 0 (bitwise) | 0 |
| `nn_rbm1bJ10` | 7952 | 4.6e-16 | 3 |
| `nn_rbm1bJ100` | 79412 | 1.1e-13 | 735 |
| `normal_mixture` | 4 | 0 (bitwise) | 0 |
| `normal_mixture_k` | 15 | 1.2e-13 | 1086 |
| `one_comp_mm_elim_abs` | 5 | 6.9e-15 | 256 |
| `pilots` | 19 | 1.4e-16 | 2 |
| `prophet` | 63 | 3.8e-15 | 21 |
| `radon_county` | 390 | 5.6e-17 | 60 |
| `radon_county_intercept` | 389 | 1.8e-14 | 136 |
| `radon_hierarchical_intercept_centered` | 392 | 8.5e-14 | 407 |
| `radon_hierarchical_intercept_noncentered` | 392 | 1.6e-14 | 96 |
| `radon_partially_pooled_centered` | 390 | 3.4e-14 | 229 |
| `radon_partially_pooled_noncentered` | 390 | 1.6e-14 | 152 |
| `radon_pooled` | 4 | 2.1e-14 | 140 |
| `radon_variable_intercept_centered` | 391 | 1.7e-14 | 105 |
| `radon_variable_intercept_noncentered` | 391 | 2.2e-14 | 288 |
| `radon_variable_intercept_slope_centered` | 778 | 2.8e-14 | 201 |
| `radon_variable_intercept_slope_noncentered` | 778 | 4.4e-15 | 40 |
| `radon_variable_slope_centered` | 391 | 3.0e-14 | 168 |
| `radon_variable_slope_noncentered` | 391 | 1.5e-14 | 848 |
| `rats_model` | 66 | 4.1e-16 | 3 |
| `seeds_centered_model` | 27 | 0 (bitwise) | 0 |
| `seeds_model` | 27 | 0 (bitwise) | 0 |
| `seeds_stanified_model` | 27 | 0 (bitwise) | 0 |
| `sesame_one_pred_a` | 4 | 0 (bitwise) | 0 |
| `soil_incubation` | 7 | 1.6e-13 | 1241 |
| `state_space_stochastic_level_stochastic_seasonal` | 390 | 5.8e-16 | 12 |
| `surgical_model` | 15 | 0 (bitwise) | 0 |
| `wells_daae_c_model` | 7 | 8.6e-15 | 55 |
| `wells_dae_c_model` | 6 | 9.2e-14 | 446 |
| `wells_dae_inter_model` | 8 | 2.7e-14 | 224 |
| `wells_dae_model` | 5 | 0 (bitwise) | 0 |
| `wells_dist` | 3 | 0 (bitwise) | 0 |
| `wells_dist100_model` | 3 | 0 (bitwise) | 0 |
| `wells_dist100ars_model` | 4 | 0 (bitwise) | 0 |
| `wells_interaction_c_model` | 5 | 6.6e-15 | 39 |
| `wells_interaction_model` | 5 | 0 (bitwise) | 0 |

Models over the default budget:

- `dogs`: 31 and 32 ULP from CmdStan at two of the three recorded points, against a 30 ULP budget. Against a 60-digit reference both engines are off by about as much: CmdStan sums the 750 Bernoulli terms one at a time and lands 10 to 59 ULP from the true log density, stanli's one merged call uses Eigen's packet reduction and vectorized exp/log1p and lands 15 to 63 ULP off, and on the gradient each engine is the closer one at a different point. Matching CmdStan would mean adopting its order; pairwise summation would put the merged call within 1 ULP of the reference at a larger distance from CmdStan.
- `dogs_log`: bitwise at the primary point and 25 ULP at another recorded point, for the same reason as dogs, inside the 30 ULP budget.

## write_array references

The oracle also records CmdStan's write_array at the same point: every CSV column (constrained parameters, transformed parameters, generated quantities). Both direct-write-array drivers use Stan's RNG with the same seed and chain 0, so generated-quantity draws are compared too. tools/verify_refs.py replays the rows in CI with column names matched exactly and values sharing the model's gate.

| model | write_array values compared |
| --- | ---: |
| `2pl_latent_reg_irt` | 549 |
| `GLMM1_model` | 2395 |
| `GLMM_Poisson_model` | 125 |
| `GLM_Binomial_model` | 83 |
| `GLM_Poisson_model` | 84 |
| `M0_model` | 4 |
| `Mb_model` | 1596 |
| `Mh_model` | 1159 |
| `Mt_model` | 7 |
| `Mtbh_model` | 1912 |
| `Mth_model` | 5044 |
| `Rate_1_model` | 1 |
| `Rate_2_model` | 3 |
| `Rate_3_model` | 1 |
| `Rate_4_model` | 4 |
| `Rate_5_model` | 3 |
| `Survey_model` | 1002 |
| `accel_gp` | 72 |
| `accel_splines` | 160 |
| `arK` | 7 |
| `arma11` | 4 |
| `blr` | 6 |
| `bones_model` | 13 |
| `bym2_offset_only` | 9610 |
| `covid19imperial_v2` | 8457 |
| `covid19imperial_v3` | 8457 |
| `diamonds` | 27 |
| `dogs` | 2253 |
| `dogs_hierarchical` | 752 |
| `dogs_log` | 2252 |
| `dogs_nonhierarchical` | 946 |
| `dugongs_model` | 6 |
| `earn_height` | 3 |
| `eight_schools_centered` | 10 |
| `eight_schools_noncentered` | 18 |
| `election88_full` | 11656 |
| `garch11` | 4 |
| `gp_pois_regr` | 24 |
| `gp_regr` | 3 |
| `gpcm_latent_reg_irt` | 550 |
| `grsm_latent_reg_irt` | 418 |
| `hier_2pl` | 804 |
| `hierarchical_gp` | 3279 |
| `hmm_drive_0` | 429 |
| `hmm_drive_1` | 429 |
| `hmm_example` | 111 |
| `hmm_gaussian` | 9519 |
| `iohmm_reg` | 18031 |
| `irt_2pl` | 144 |
| `kidscore_interaction` | 5 |
| `kidscore_interaction_c` | 5 |
| `kidscore_interaction_c2` | 5 |
| `kidscore_interaction_z` | 5 |
| `kidscore_mom_work` | 5 |
| `kidscore_momhs` | 3 |
| `kidscore_momhsiq` | 4 |
| `kidscore_momiq` | 3 |
| `kilpisjarvi` | 3 |
| `ldaK2` | 12 |
| `ldaK5` | 7780 |
| `log10earn_height` | 3 |
| `logearn_height` | 3 |
| `logearn_height_male` | 4 |
| `logearn_interaction` | 5 |
| `logearn_interaction_z` | 5 |
| `logearn_logheight_male` | 4 |
| `logistic_regression_rhs` | 4719 |
| `logmesquite` | 8 |
| `logmesquite_logva` | 5 |
| `logmesquite_logvas` | 8 |
| `logmesquite_logvash` | 7 |
| `logmesquite_logvolume` | 3 |
| `losscurve_sislob` | 384 |
| `lotka_volterra` | 90 |
| `low_dim_gauss_mix` | 5 |
| `low_dim_gauss_mix_collapse` | 5 |
| `lsat_model` | 1012 |
| `mesquite` | 8 |
| `multi_occupancy` | 312 |
| `nes` | 10 |
| `nes_logit_model` | 2 |
| `nn_rbm1bJ10` | 7951 |
| `nn_rbm1bJ100` | 79411 |
| `normal_mixture` | 3 |
| `normal_mixture_k` | 15 |
| `one_comp_mm_elim_abs` | 44 |
| `pilots` | 58 |
| `prophet` | 62 |
| `radon_county` | 389 |
| `radon_county_intercept` | 388 |
| `radon_hierarchical_intercept_centered` | 391 |
| `radon_hierarchical_intercept_noncentered` | 777 |
| `radon_partially_pooled_centered` | 389 |
| `radon_partially_pooled_noncentered` | 775 |
| `radon_pooled` | 3 |
| `radon_variable_intercept_centered` | 390 |
| `radon_variable_intercept_noncentered` | 776 |
| `radon_variable_intercept_slope_centered` | 777 |
| `radon_variable_intercept_slope_noncentered` | 1549 |
| `radon_variable_slope_centered` | 390 |
| `radon_variable_slope_noncentered` | 776 |
| `rats_model` | 66 |
| `seeds_centered_model` | 47 |
| `seeds_model` | 27 |
| `seeds_stanified_model` | 26 |
| `sesame_one_pred_a` | 3 |
| `soil_incubation` | 31 |
| `state_space_stochastic_level_stochastic_seasonal` | 581 |
| `surgical_model` | 28 |
| `wells_daae_c_model` | 6 |
| `wells_dae_c_model` | 5 |
| `wells_dae_inter_model` | 7 |
| `wells_dae_model` | 4 |
| `wells_dist` | 2 |
| `wells_dist100_model` | 2 |
| `wells_dist100ars_model` | 3 |
| `wells_interaction_c_model` | 4 |
| `wells_interaction_model` | 4 |

## Rejected by both engines

CmdStan and stanli both reject every shared evaluation point for these models: the model is invalid there (an ODE solution dipping below a declared lower bound, for instance), so there is nothing to compare. Agreement, not a gap, but not counted as verified either.

- `sir`

## Evaluate but not verified

- `kronecker_gp`: max rel diff 6.3e-03
  - lp matches CmdStan to 1e-13 and 436/438 gradients match; the two that flow through eigenvectors_sym differ by 0.7%. The covariance at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest 6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - lambda_j), so last-bit differences in the input are amplified by ~1e16. Every component op (eigen decomposition, transpose, matrix product, the whole chain with one operand held constant) matches CmdStan bitwise in isolation.

## Failures

- `sir`: EVAL_FAIL stanli MIR check: y is -1.82492e-07, but must be greater than or equal to 0.000000

## Rethinking teaching corpus

62/62 fixtures verified at all three CmdStan points. The inventory covers all 61 ulam call sites in chapters 4–16 of the second edition, plus a supplemental hurdle model. Counts here are separate from posteriordb. These are the recorder's measurements; `tools/verify_refs.py` replays them against the current build in CI.

See [the inventory and provenance](../tests/rethinking/README.md).

| model | verified points | worst scaled error |
| --- | ---: | ---: |
| `ch09_m5_8s` | 3/3 | 7.23e-16 |
| `ch09_m5_8s2` | 3/3 | 5.36e-16 |
| `ch09_m9_1` | 3/3 | 8.88e-16 |
| `ch09_m9_1_chains4` | 3/3 | 8.88e-16 |
| `ch09_m9_2` | 3/3 | 0.00e+00 |
| `ch09_m9_3` | 3/3 | 0.00e+00 |
| `ch09_m9_4` | 3/3 | 0.00e+00 |
| `ch09_m9_5` | 3/3 | 0.00e+00 |
| `ch09_mp` | 3/3 | 0.00e+00 |
| `ch11_m11_10` | 3/3 | 0.00e+00 |
| `ch11_m11_11` | 3/3 | 2.78e-16 |
| `ch11_m11_4` | 3/3 | 1.70e-15 |
| `ch11_m11_5` | 3/3 | 5.46e-15 |
| `ch11_m11_6` | 3/3 | 0.00e+00 |
| `ch11_m11_7` | 3/3 | 3.54e-16 |
| `ch11_m11_8` | 3/3 | 0.00e+00 |
| `ch11_m11_9` | 3/3 | 0.00e+00 |
| `ch11_m_pois` | 3/3 | 0.00e+00 |
| `ch12_m12_1` | 3/3 | 3.36e-16 |
| `ch12_m12_2` | 3/3 | 2.09e-16 |
| `ch12_m12_3` | 3/3 | 9.10e-15 |
| `ch12_m12_3_alt` | 3/3 | 1.81e-14 |
| `ch12_m12_4` | 3/3 | 6.50e-14 |
| `ch12_m12_5` | 3/3 | 2.00e-14 |
| `ch12_m12_6` | 3/3 | 2.90e-14 |
| `ch12_m12_7` | 3/3 | 2.61e-14 |
| `ch13_m13_1` | 3/3 | 0.00e+00 |
| `ch13_m13_2` | 3/3 | 0.00e+00 |
| `ch13_m13_3` | 3/3 | 0.00e+00 |
| `ch13_m13_4` | 3/3 | 7.69e-15 |
| `ch13_m13_4b` | 3/3 | 7.69e-15 |
| `ch13_m13_4nc` | 3/3 | 4.05e-15 |
| `ch13_m13_5` | 3/3 | 7.18e-15 |
| `ch13_m13_6` | 3/3 | 4.36e-15 |
| `ch13_m13_7` | 3/3 | 0.00e+00 |
| `ch13_m13_7nc` | 3/3 | 0.00e+00 |
| `ch14_m14_1` | 3/3 | 7.57e-16 |
| `ch14_m14_10` | 3/3 | 1.82e-15 |
| `ch14_m14_11` | 3/3 | 1.48e-13 |
| `ch14_m14_2` | 3/3 | 1.24e-15 |
| `ch14_m14_3` | 3/3 | 1.28e-15 |
| `ch14_m14_4` | 3/3 | 8.88e-16 |
| `ch14_m14_4x` | 3/3 | 8.88e-16 |
| `ch14_m14_5` | 3/3 | 8.88e-16 |
| `ch14_m14_6` | 3/3 | 4.03e-15 |
| `ch14_m14_6x` | 3/3 | 4.03e-15 |
| `ch14_m14_7` | 3/3 | 2.58e-15 |
| `ch14_m14_8` | 3/3 | 3.00e-16 |
| `ch14_m14_8nc` | 3/3 | 4.93e-16 |
| `ch14_m14_9` | 3/3 | 5.55e-15 |
| `ch15_m15_1` | 3/3 | 5.29e-16 |
| `ch15_m15_2` | 3/3 | 2.21e-16 |
| `ch15_m15_3` | 3/3 | 0.00e+00 |
| `ch15_m15_4` | 3/3 | 0.00e+00 |
| `ch15_m15_5` | 3/3 | 1.11e-15 |
| `ch15_m15_6` | 3/3 | 7.94e-16 |
| `ch15_m15_7` | 3/3 | 4.00e-15 |
| `ch15_m15_8` | 3/3 | 4.21e-15 |
| `ch15_m15_9` | 3/3 | 4.21e-15 |
| `ch16_m16_1` | 3/3 | 1.81e-15 |
| `ch16_m16_4` | 3/3 | 1.71e-15 |
| `extra_hurdle_poisson` | 3/3 | 2.17e-16 |
