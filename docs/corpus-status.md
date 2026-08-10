# Corpus status

Evaluating: 119/120
Differentially verified against CmdStan: 118/120

A model counts as passing only when tools/verify_sample.py matches CmdStan's log_prob and full gradient at the shared deterministic point. Accuracy below is the worst deviation over lp and every gradient component: relative, and in ULPs (0 = bitwise identical to CmdStan). Models that evaluate but are not verified are listed separately and are not counted.

| model | values compared | max rel diff | max ULP |
| --- | ---: | ---: | ---: |
| `2pl_latent_reg_irt` | 532 | 2.0e-15 | 64 |
| `GLMM1_model` | 238 | 1.5e-14 | 104 |
| `GLMM_Poisson_model` | 46 | 0 (bitwise) | 0 |
| `GLM_Binomial_model` | 4 | 0 (bitwise) | 0 |
| `GLM_Poisson_model` | 5 | 1.2e-16 | 1 |
| `M0_model` | 3 | 1.1e-14 | 62 |
| `Mb_model` | 4 | 1.2e-14 | 68 |
| `Mh_model` | 389 | 2.5e-15 | 19 |
| `Mt_model` | 5 | 1.7e-15 | 10 |
| `Mtbh_model` | 155 | 1.2e-15 | 23 |
| `Mth_model` | 395 | 2.4e-15 | 96 |
| `Rate_1_model` | 2 | 0 (bitwise) | 0 |
| `Rate_2_model` | 3 | 0 (bitwise) | 0 |
| `Rate_3_model` | 2 | 0 (bitwise) | 0 |
| `Rate_4_model` | 3 | 1.3e-16 | 1 |
| `Rate_5_model` | 2 | 0 (bitwise) | 0 |
| `Survey_model` | 2 | 0 (bitwise) | 0 |
| `accel_gp` | 67 | 1.6e-15 | 11 |
| `accel_splines` | 83 | 6.6e-15 | 119 |
| `arK` | 8 | 9.6e-16 | 5 |
| `arma11` | 5 | 0 (bitwise) | 0 |
| `blr` | 7 | 5.1e-16 | 3 |
| `bones_model` | 14 | 1.5e-16 | 1 |
| `bym2_offset_only` | 3846 | 2.0e-15 | 18 |
| `covid19imperial_v2` | 52 | 8.2e-16 | 7 |
| `covid19imperial_v3` | 52 | 8.2e-16 | 7 |
| `diamonds` | 27 | 2.6e-12 | 16248 |
| `dogs` | 4 | 4.6e-15 | 26 |
| `dogs_hierarchical` | 3 | 1.4e-16 | 1 |
| `dogs_log` | 3 | 0 (bitwise) | 0 |
| `dogs_nonhierarchical` | 66 | 7.4e-16 | 5 |
| `dugongs_model` | 5 | 1.9e-16 | 1 |
| `earn_height` | 4 | 0 (bitwise) | 0 |
| `eight_schools_centered` | 11 | 0 (bitwise) | 0 |
| `eight_schools_noncentered` | 11 | 0 (bitwise) | 0 |
| `election88_full` | 91 | 9.8e-15 | 81 |
| `garch11` | 5 | 7.1e-16 | 4 |
| `gp_pois_regr` | 14 | 0 (bitwise) | 0 |
| `gp_regr` | 4 | 0 (bitwise) | 0 |
| `gpcm_latent_reg_irt` | 531 | 1.5e-14 | 3928 |
| `grsm_latent_reg_irt` | 409 | 1.3e-15 | 11 |
| `hier_2pl` | 670 | 0 (bitwise) | 0 |
| `hierarchical_gp` | 934 | 7.0e-16 | 88 |
| `hmm_drive_0` | 7 | 1.2e-16 | 1 |
| `hmm_drive_1` | 7 | 1.4e-16 | 1 |
| `hmm_example` | 5 | 0 (bitwise) | 0 |
| `hmm_gaussian` | 15 | 0 (bitwise) | 0 |
| `iohmm_reg` | 30 | 2.2e-14 | 292 |
| `irt_2pl` | 145 | 0 (bitwise) | 0 |
| `kidscore_interaction` | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_c` | 6 | 4.7e-14 | 330 |
| `kidscore_interaction_c2` | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_z` | 6 | 5.6e-14 | 363 |
| `kidscore_mom_work` | 6 | 0 (bitwise) | 0 |
| `kidscore_momhs` | 4 | 0 (bitwise) | 0 |
| `kidscore_momhsiq` | 5 | 0 (bitwise) | 0 |
| `kidscore_momiq` | 4 | 0 (bitwise) | 0 |
| `kilpisjarvi` | 4 | 0 (bitwise) | 0 |
| `ldaK2` | 8 | 9.6e-15 | 57 |
| `ldaK5` | 7715 | 1.8e-15 | 15 |
| `log10earn_height` | 4 | 0 (bitwise) | 0 |
| `logearn_height` | 4 | 4.3e-16 | 2 |
| `logearn_height_male` | 5 | 0 (bitwise) | 0 |
| `logearn_interaction` | 6 | 2.6e-16 | 2 |
| `logearn_interaction_z` | 6 | 1.1e-15 | 9 |
| `logearn_logheight_male` | 5 | 2.1e-16 | 1 |
| `logistic_regression_rhs` | 3076 | 6.6e-16 | 5 |
| `logmesquite` | 9 | 3.9e-16 | 3 |
| `logmesquite_logva` | 6 | 1.3e-16 | 1 |
| `logmesquite_logvas` | 9 | 2.0e-16 | 1 |
| `logmesquite_logvash` | 8 | 3.4e-16 | 2 |
| `logmesquite_logvolume` | 4 | 1.7e-16 | 1 |
| `losscurve_sislob` | 16 | 1.6e-16 | 1 |
| `lotka_volterra` | 9 | 0 (bitwise) | 0 |
| `low_dim_gauss_mix` | 6 | 0 (bitwise) | 0 |
| `low_dim_gauss_mix_collapse` | 6 | 0 (bitwise) | 0 |
| `lsat_model` | 1007 | 1.3e-16 | 1 |
| `mesquite` | 9 | 0 (bitwise) | 0 |
| `multi_occupancy` | 107 | 4.8e-15 | 29 |
| `nes` | 11 | 0 (bitwise) | 0 |
| `nes_logit_model` | 3 | 0 (bitwise) | 0 |
| `nn_rbm1bJ10` | 7952 | 4.6e-16 | 3 |
| `nn_rbm1bJ100` | 79412 | 1.1e-13 | 735 |
| `normal_mixture` | 4 | 0 (bitwise) | 0 |
| `normal_mixture_k` | 15 | 1.1e-14 | 57 |
| `one_comp_mm_elim_abs` | 5 | 6.8e-15 | 256 |
| `pilots` | 19 | 1.4e-16 | 2 |
| `prophet` | 63 | 8.1e-15 | 46 |
| `radon_county` | 390 | 2.8e-17 | 368 |
| `radon_county_intercept` | 389 | 1.8e-14 | 136 |
| `radon_hierarchical_intercept_centered` | 392 | 8.5e-14 | 404 |
| `radon_hierarchical_intercept_noncentered` | 392 | 1.6e-14 | 96 |
| `radon_partially_pooled_centered` | 390 | 3.4e-14 | 229 |
| `radon_partially_pooled_noncentered` | 390 | 1.6e-14 | 75 |
| `radon_pooled` | 4 | 2.1e-14 | 140 |
| `radon_variable_intercept_centered` | 391 | 1.7e-14 | 105 |
| `radon_variable_intercept_noncentered` | 391 | 2.2e-14 | 288 |
| `radon_variable_intercept_slope_centered` | 778 | 2.8e-14 | 201 |
| `radon_variable_intercept_slope_noncentered` | 778 | 2.6e-15 | 24 |
| `radon_variable_slope_centered` | 391 | 3.0e-14 | 168 |
| `radon_variable_slope_noncentered` | 391 | 1.5e-14 | 144 |
| `rats_model` | 66 | 4.1e-16 | 3 |
| `seeds_centered_model` | 27 | 0 (bitwise) | 0 |
| `seeds_model` | 27 | 0 (bitwise) | 0 |
| `seeds_stanified_model` | 27 | 0 (bitwise) | 0 |
| `sesame_one_pred_a` | 4 | 0 (bitwise) | 0 |
| `soil_incubation` | 7 | 1.3e-16 | 1 |
| `state_space_stochastic_level_stochastic_seasonal` | 390 | 8.7e-16 | 6 |
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

## write_array references

For models whose generated quantities are deterministic (no `_rng`), the oracle also records CmdStan's write_array at the same point: every CSV column (constrained parameters, transformed parameters, generated quantities). tools/verify_refs.py replays them in CI with the column names matched exactly and the values sharing the model's gate. Models with RNG draws are exercised structurally (all columns produced and finite) by harnesses/wa_coverage.py instead, since their values are a property of the RNG stream.

| model | write_array values compared |
| --- | ---: |
| `2pl_latent_reg_irt` | 549 |
| `GLMM_Poisson_model` | 125 |
| `GLM_Binomial_model` | 83 |
| `GLM_Poisson_model` | 84 |
| `accel_gp` | 72 |
| `accel_splines` | 160 |
| `bym2_offset_only` | 9610 |
| `diamonds` | 27 |
| `gpcm_latent_reg_irt` | 550 |
| `grsm_latent_reg_irt` | 418 |
| `hier_2pl` | 804 |
| `hmm_drive_0` | 429 |
| `hmm_drive_1` | 429 |
| `hmm_example` | 111 |
| `hmm_gaussian` | 9519 |
| `logistic_regression_rhs` | 4719 |
| `losscurve_sislob` | 384 |
| `lsat_model` | 1012 |
| `rats_model` | 66 |
| `surgical_model` | 28 |

## Rejected by both engines

CmdStan and stanli both reject every shared evaluation point for these models: the model is invalid there (an ODE solution dipping below a declared lower bound, for instance), so there is nothing to compare. Agreement, not a gap, but not counted as verified either.

- `sir`

## Evaluate but not verified

- `kronecker_gp`: max rel diff 7.1e-03
  - lp matches CmdStan to 1e-13 and 436/438 gradients match; the two that flow through eigenvectors_sym differ by 0.7%. The covariance at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest 6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - lambda_j), so last-bit differences in the input are amplified by ~1e16. Every component op (eigen decomposition, transpose, matrix product, the whole chain with one operand held constant) matches CmdStan bitwise in isolation.

## Failures

- `sir`: EVAL_FAIL poisson_lpmf: Rate parameter is -2.06273e-09, but must be nonnegative!
