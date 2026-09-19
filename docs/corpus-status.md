# Corpus status

Evaluating: 329/330
CmdStan reference coverage: 329/330 models, 987 evaluation points.

The shared corpus includes posteriordb, generated brms models, imported teaching models, and language fixtures. Collection labels retain their source provenance; all references use the same replay in `tools/verify_refs.py`.

Recording-time primary-point comparison metrics retained for 314 verified models are shown below. Imported references retain their original answers and per-model recording provenance, without inventing historical comparison metrics. Reference coverage is separate from a current-build numerical replay result.

A model counts as passing only when tools/verify_sample.py matches CmdStan's log_prob and full gradient at the shared deterministic point. Accuracy below is the worst deviation over lp and every gradient component: relative, and in ULPs (0 = bitwise identical to CmdStan). Bitwise counts are reported for information; the replay uses a 1e-9 scaled-error gate with documented ill-conditioned exceptions. Models that evaluate but are not verified are listed separately and are not counted.

| model | source collection | values compared | max rel diff | max ULP |
| --- | --- | ---: | ---: | ---: |
| `2pl_latent_reg_irt` | posteriordb | 532 | 0 (bitwise) | 0 |
| `GLMM1_model` | posteriordb | 238 | 1.5e-14 | 104 |
| `GLMM_Poisson_model` | posteriordb | 46 | 1.8e-15 | 16 |
| `GLM_Binomial_model` | posteriordb | 4 | 0 (bitwise) | 0 |
| `GLM_Poisson_model` | posteriordb | 5 | 2.2e-16 | 1 |
| `M0_model` | posteriordb | 3 | 9.6e-15 | 61 |
| `Mb_model` | posteriordb | 4 | 4.9e-14 | 332 |
| `Mh_model` | posteriordb | 389 | 5.5e-15 | 42 |
| `Mt_model` | posteriordb | 5 | 2.3e-14 | 132 |
| `Mtbh_model` | posteriordb | 155 | 9.7e-16 | 23 |
| `Mth_model` | posteriordb | 395 | 3.4e-15 | 96 |
| `Rate_1_model` | posteriordb | 2 | 0 (bitwise) | 0 |
| `Rate_2_model` | posteriordb | 3 | 0 (bitwise) | 0 |
| `Rate_3_model` | posteriordb | 2 | 0 (bitwise) | 0 |
| `Rate_4_model` | posteriordb | 3 | 1.3e-16 | 1 |
| `Rate_5_model` | posteriordb | 2 | 0 (bitwise) | 0 |
| `Survey_model` | posteriordb | 2 | 7.5e-15 | 54 |
| `aalto_bern` | educational | 2 | not recorded | not recorded |
| `aalto_binom` | educational | 2 | not recorded | not recorded |
| `aalto_binom2` | educational | 3 | not recorded | not recorded |
| `aalto_binomb` | educational | 2 | not recorded | not recorded |
| `aalto_gpareto` | educational | 3 | not recorded | not recorded |
| `aalto_grp_aov` | educational | 5 | not recorded | not recorded |
| `aalto_grp_prior_mean` | educational | 7 | not recorded | not recorded |
| `aalto_grp_prior_mean_var` | educational | 11 | not recorded | not recorded |
| `aalto_lin` | educational | 4 | not recorded | not recorded |
| `aalto_lin_std` | educational | 4 | not recorded | not recorded |
| `aalto_lin_std_t` | educational | 5 | not recorded | not recorded |
| `aalto_poisson_hurdle` | educational | 3 | not recorded | not recorded |
| `aalto_poisson_simple` | educational | 2 | not recorded | not recorded |
| `accel_gp` | posteriordb | 67 | 3.4e-16 | 2 |
| `accel_splines` | posteriordb | 83 | 3.1e-16 | 2 |
| `arK` | posteriordb | 8 | 9.6e-16 | 5 |
| `arma11` | posteriordb | 5 | 0 (bitwise) | 0 |
| `blr` | posteriordb | 7 | 1.8e-16 | 1 |
| `bones_model` | posteriordb | 14 | 1.5e-16 | 1 |
| `bym2_offset_only` | posteriordb | 3846 | 1.3e-15 | 8 |
| `ch09_m5_8s` | rethinking | 5 | 7.2e-16 | 4 |
| `ch09_m5_8s2` | rethinking | 5 | 2.7e-16 | 2 |
| `ch09_m9_1` | rethinking | 6 | 1.5e-16 | 1 |
| `ch09_m9_1_chains4` | rethinking | 6 | 1.5e-16 | 1 |
| `ch09_m9_2` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch09_m9_3` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch09_m9_4` | rethinking | 4 | 0 (bitwise) | 0 |
| `ch09_m9_5` | rethinking | 4 | 0 (bitwise) | 0 |
| `ch09_mp` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch11_m11_10` | rethinking | 5 | 0 (bitwise) | 0 |
| `ch11_m11_11` | rethinking | 6 | 2.8e-16 | 2 |
| `ch11_m11_4` | rethinking | 12 | 1.5e-15 | 8 |
| `ch11_m11_5` | rethinking | 12 | 5.5e-15 | 37 |
| `ch11_m11_6` | rethinking | 12 | 0 (bitwise) | 0 |
| `ch11_m11_7` | rethinking | 3 | 2.6e-16 | 2 |
| `ch11_m11_8` | rethinking | 9 | 0 (bitwise) | 0 |
| `ch11_m11_9` | rethinking | 2 | 0 (bitwise) | 0 |
| `ch11_m_pois` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch12_m12_1` | rethinking | 4 | 1.3e-16 | 1 |
| `ch12_m12_2` | rethinking | 7 | 2.1e-16 | 1 |
| `ch12_m12_3` | rethinking | 3 | 4.3e-15 | 35 |
| `ch12_m12_3_alt` | rethinking | 3 | 4.0e-15 | 30 |
| `ch12_m12_4` | rethinking | 7 | 2.7e-14 | 207 |
| `ch12_m12_5` | rethinking | 12 | 1.8e-14 | 159 |
| `ch12_m12_6` | rethinking | 17 | 2.9e-14 | 230 |
| `ch12_m12_7` | rethinking | 11 | 2.6e-14 | 232 |
| `ch13_m13_1` | rethinking | 49 | 0 (bitwise) | 0 |
| `ch13_m13_2` | rethinking | 51 | 0 (bitwise) | 0 |
| `ch13_m13_3` | rethinking | 63 | 0 (bitwise) | 0 |
| `ch13_m13_4` | rethinking | 21 | 7.7e-15 | 36 |
| `ch13_m13_4b` | rethinking | 21 | 7.7e-15 | 36 |
| `ch13_m13_4nc` | rethinking | 21 | 4.1e-15 | 33 |
| `ch13_m13_5` | rethinking | 14 | 7.2e-15 | 44 |
| `ch13_m13_6` | rethinking | 22 | 4.4e-15 | 36 |
| `ch13_m13_7` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch13_m13_7nc` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch14_m14_1` | rethinking | 47 | 7.6e-16 | 6 |
| `ch14_m14_10` | rethinking | 5 | 1.8e-15 | 9 |
| `ch14_m14_11` | rethinking | 6 | 6.0e-15 | 37 |
| `ch14_m14_2` | rethinking | 77 | 1.0e-15 | 297 |
| `ch14_m14_3` | rethinking | 77 | 1.3e-15 | 96 |
| `ch14_m14_4` | rethinking | 4 | 5.8e-16 | 5 |
| `ch14_m14_4x` | rethinking | 4 | 5.8e-16 | 5 |
| `ch14_m14_5` | rethinking | 5 | 6.7e-16 | 4 |
| `ch14_m14_6` | rethinking | 8 | 2.3e-15 | 11 |
| `ch14_m14_6x` | rethinking | 8 | 2.3e-15 | 11 |
| `ch14_m14_7` | rethinking | 657 | 1.1e-15 | 9 |
| `ch14_m14_8` | rethinking | 16 | 1.5e-16 | 1 |
| `ch14_m14_8nc` | rethinking | 16 | 4.9e-16 | 4 |
| `ch14_m14_9` | rethinking | 5 | 3.4e-16 | 3 |
| `ch15_m15_1` | rethinking | 55 | 2.6e-16 | 64 |
| `ch15_m15_2` | rethinking | 105 | 1.9e-16 | 1 |
| `ch15_m15_3` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch15_m15_4` | rethinking | 3 | 0 (bitwise) | 0 |
| `ch15_m15_5` | rethinking | 19 | 9.2e-16 | 33 |
| `ch15_m15_6` | rethinking | 7 | 5.7e-16 | 4 |
| `ch15_m15_7` | rethinking | 22 | 4.0e-15 | 144 |
| `ch15_m15_8` | rethinking | 4 | 4.7e-15 | 21 |
| `ch15_m15_9` | rethinking | 4 | 4.7e-15 | 21 |
| `ch16_m16_1` | rethinking | 4 | 1.8e-15 | 15 |
| `ch16_m16_4` | rethinking | 4 | 1.7e-15 | 8 |
| `cholesky_cov_param_block` | stanc3 | 21 | 0 (bitwise) | 0 |
| `covid19imperial_v2` | posteriordb | 52 | 8.2e-16 | 7 |
| `covid19imperial_v3` | posteriordb | 52 | 8.2e-16 | 7 |
| `declare-define-multi` | stanc3 | 376 | 0 (bitwise) | 0 |
| `diamonds` | posteriordb | 27 | 0 (bitwise) | 0 |
| `dogs` | posteriordb | 4 | 5.5e-15 | 31 |
| `dogs_hierarchical` | posteriordb | 3 | 1.2e-15 | 7 |
| `dogs_log` | posteriordb | 3 | 0 (bitwise) | 0 |
| `dogs_nonhierarchical` | posteriordb | 66 | 6.9e-16 | 4 |
| `dugongs_model` | posteriordb | 5 | 1.9e-16 | 1 |
| `earn_height` | posteriordb | 4 | 0 (bitwise) | 0 |
| `eight_schools_centered` | posteriordb | 11 | 0 (bitwise) | 0 |
| `eight_schools_noncentered` | posteriordb | 11 | 0 (bitwise) | 0 |
| `election88_full` | posteriordb | 91 | 9.9e-15 | 82 |
| `extra_hurdle_poisson` | rethinking | 3 | 2.2e-16 | 1 |
| `garch11` | posteriordb | 5 | 1.6e-15 | 8 |
| `gp_pois_regr` | posteriordb | 14 | 0 (bitwise) | 0 |
| `gp_regr` | posteriordb | 4 | 0 (bitwise) | 0 |
| `gpcm_latent_reg_irt` | posteriordb | 531 | 2.6e-13 | 4608 |
| `grsm_latent_reg_irt` | posteriordb | 409 | 1.2e-14 | 81 |
| `hier_2pl` | posteriordb | 670 | 0 (bitwise) | 0 |
| `hierarchical_gp` | posteriordb | 934 | 7.0e-16 | 88 |
| `hmm_drive_0` | posteriordb | 7 | 6.2e-16 | 3 |
| `hmm_drive_1` | posteriordb | 7 | 1.0e-15 | 8 |
| `hmm_example` | posteriordb | 5 | 4.6e-16 | 4 |
| `hmm_gaussian` | posteriordb | 15 | 7.0e-15 | 36 |
| `i319_gauss_re` | brms | 67 | 1.1e-16 | 1 |
| `i319_negbin_fixed` | brms | 7 | 0 (bitwise) | 0 |
| `i319_negbin_re` | brms | 67 | 0 (bitwise) | 0 |
| `i319_pois_fixed` | brms | 6 | 0 (bitwise) | 0 |
| `i319_pois_re` | brms | 66 | 2.3e-16 | 2 |
| `i319_pois_re2` | brms | 303 | 1.8e-16 | 1 |
| `i320_gp_expquad` | brms | 35 | 0 (bitwise) | 0 |
| `i320_gp_matern32` | brms | 35 | 3.6e-14 | 256 |
| `i320_mi_nhanes` | brms | 28 | 1.4e-17 | 1 |
| `i320_pois_trunc_both` | brms | 5 | 1.7e-15 | 13 |
| `i320_pois_trunc_ub` | brms | 5 | 1.7e-15 | 13 |
| `i320_sratio_cs` | brms | 9 | 1.8e-15 | 66 |
| `i320_sratio_plain` | brms | 7 | 1.5e-15 | 7 |
| `iohmm_reg` | posteriordb | 30 | 2.3e-14 | 2768 |
| `irt_2pl` | posteriordb | 145 | 0 (bitwise) | 0 |
| `kidscore_interaction` | posteriordb | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_c` | posteriordb | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_c2` | posteriordb | 6 | 0 (bitwise) | 0 |
| `kidscore_interaction_z` | posteriordb | 6 | 0 (bitwise) | 0 |
| `kidscore_mom_work` | posteriordb | 6 | 0 (bitwise) | 0 |
| `kidscore_momhs` | posteriordb | 4 | 0 (bitwise) | 0 |
| `kidscore_momhsiq` | posteriordb | 5 | 1.4e-16 | 1 |
| `kidscore_momiq` | posteriordb | 4 | 0 (bitwise) | 0 |
| `kilpisjarvi` | posteriordb | 4 | 0 (bitwise) | 0 |
| `ldaK2` | posteriordb | 8 | 6.8e-14 | 452 |
| `ldaK5` | posteriordb | 7715 | 7.1e-13 | 10698848 |
| `log10earn_height` | posteriordb | 4 | 0 (bitwise) | 0 |
| `logearn_height` | posteriordb | 4 | 0 (bitwise) | 0 |
| `logearn_height_male` | posteriordb | 5 | 0 (bitwise) | 0 |
| `logearn_interaction` | posteriordb | 6 | 0 (bitwise) | 0 |
| `logearn_interaction_z` | posteriordb | 6 | 0 (bitwise) | 0 |
| `logearn_logheight_male` | posteriordb | 5 | 0 (bitwise) | 0 |
| `logistic_regression_rhs` | posteriordb | 3076 | 2.6e-15 | 2048 |
| `logmesquite` | posteriordb | 9 | 0 (bitwise) | 0 |
| `logmesquite_logva` | posteriordb | 6 | 0 (bitwise) | 0 |
| `logmesquite_logvas` | posteriordb | 9 | 0 (bitwise) | 0 |
| `logmesquite_logvash` | posteriordb | 8 | 0 (bitwise) | 0 |
| `logmesquite_logvolume` | posteriordb | 4 | 0 (bitwise) | 0 |
| `losscurve_sislob` | posteriordb | 16 | 4.0e-16 | 2 |
| `lotka_volterra` | posteriordb | 9 | 4.3e-15 | 22 |
| `low_dim_gauss_mix` | posteriordb | 6 | 0 (bitwise) | 0 |
| `low_dim_gauss_mix_collapse` | posteriordb | 6 | 0 (bitwise) | 0 |
| `lsat_model` | posteriordb | 1007 | 2.7e-15 | 96 |
| `lupdf-inlining` | stanc3 | 2 | 0 (bitwise) | 0 |
| `mesquite` | posteriordb | 9 | 0 (bitwise) | 0 |
| `mother` | stanc3 | 197 | 5.9e-16 | 5 |
| `multi_occupancy` | posteriordb | 107 | 4.8e-15 | 36 |
| `multidim_var_param_ar45_mat23` | stanc3 | 121 | 6.8e-16 | 4 |
| `nes` | posteriordb | 11 | 0 (bitwise) | 0 |
| `nes_logit_model` | posteriordb | 3 | 0 (bitwise) | 0 |
| `nn_rbm1bJ10` | posteriordb | 7952 | 4.6e-16 | 3 |
| `nn_rbm1bJ100` | posteriordb | 79412 | 1.1e-13 | 735 |
| `normal_mixture` | posteriordb | 4 | 0 (bitwise) | 0 |
| `normal_mixture_k` | posteriordb | 15 | 1.2e-13 | 1086 |
| `one_comp_mm_elim_abs` | posteriordb | 5 | 6.9e-15 | 256 |
| `operators` | stanc3 | 25 | 0 (bitwise) | 0 |
| `pilots` | posteriordb | 19 | 1.4e-16 | 2 |
| `prophet` | posteriordb | 63 | 3.8e-15 | 21 |
| `radon_county` | posteriordb | 390 | 5.6e-17 | 60 |
| `radon_county_intercept` | posteriordb | 389 | 1.8e-14 | 136 |
| `radon_hierarchical_intercept_centered` | posteriordb | 392 | 8.5e-14 | 407 |
| `radon_hierarchical_intercept_noncentered` | posteriordb | 392 | 1.6e-14 | 96 |
| `radon_partially_pooled_centered` | posteriordb | 390 | 3.4e-14 | 229 |
| `radon_partially_pooled_noncentered` | posteriordb | 390 | 1.6e-14 | 152 |
| `radon_pooled` | posteriordb | 4 | 2.1e-14 | 140 |
| `radon_variable_intercept_centered` | posteriordb | 391 | 1.7e-14 | 105 |
| `radon_variable_intercept_noncentered` | posteriordb | 391 | 2.2e-14 | 288 |
| `radon_variable_intercept_slope_centered` | posteriordb | 778 | 2.8e-14 | 201 |
| `radon_variable_intercept_slope_noncentered` | posteriordb | 778 | 4.4e-15 | 40 |
| `radon_variable_slope_centered` | posteriordb | 391 | 3.0e-14 | 168 |
| `radon_variable_slope_noncentered` | posteriordb | 391 | 1.5e-14 | 848 |
| `rats_model` | posteriordb | 66 | 4.1e-16 | 3 |
| `reductions_allowed` | stanc3 | 151 | 0 (bitwise) | 0 |
| `s2_ar_cov` | brms | 5 | 3.3e-16 | 2 |
| `s2_beta_binomial` | brms | 4 | 2.2e-16 | 2 |
| `s2_car` | brms | 11 | 1.6e-16 | 4 |
| `s2_car_esicar` | brms | 9 | 1.9e-16 | 1 |
| `s2_car_icar` | brms | 10 | 1.4e-16 | 1 |
| `s2_categorical_re` | brms | 17 | 8.9e-16 | 16 |
| `s2_cens_interval` | brms | 4 | 3.3e-16 | 3 |
| `s2_com_poisson` | brms | 4 | 7.5e-16 | 6 |
| `s2_cosy` | brms | 5 | 0 (bitwise) | 0 |
| `s2_cox` | brms | 7 | 1.3e-15 | 12 |
| `s2_cox_cens` | brms | 7 | 1.6e-16 | 1 |
| `s2_cumulative_cauchit` | brms | 5 | 5.6e-16 | 4 |
| `s2_cumulative_cloglog` | brms | 5 | 2.6e-16 | 2 |
| `s2_cumulative_probit` | brms | 5 | 3.3e-16 | 2 |
| `s2_custom_vint` | brms | 4 | 2.2e-16 | 2 |
| `s2_custom_vreal` | brms | 4 | 3.6e-16 | 2 |
| `s2_dirichlet` | brms | 6 | 2.7e-16 | 2 |
| `s2_discrete_weibull` | brms | 4 | 2.0e-15 | 16 |
| `s2_dist_sigma_re` | brms | 10 | 2.2e-16 | 2 |
| `s2_fcor` | brms | 4 | 2.8e-16 | 2 |
| `s2_frechet` | brms | 4 | 2.0e-16 | 1 |
| `s2_gev` | brms | 5 | 7.5e-16 | 4 |
| `s2_gp_approx` | brms | 10 | 0 (bitwise) | 0 |
| `s2_gp_by_approx` | brms | 17 | 1.6e-16 | 1 |
| `s2_gp_by_gr` | brms | 53 | 4.4e-16 | 8 |
| `s2_gr_by` | brms | 11 | 1.9e-16 | 1 |
| `s2_gr_student` | brms | 16 | 0 (bitwise) | 0 |
| `s2_hurdle_cumulative` | brms | 5 | 2.2e-16 | 2 |
| `s2_hurdle_negbin` | brms | 5 | 1.0e-15 | 8 |
| `s2_index_mi` | brms | 10 | 0 (bitwise) | 0 |
| `s2_logistic_normal` | brms | 8 | 3.5e-16 | 3 |
| `s2_me2` | brms | 90 | 1.4e-16 | 1 |
| `s2_me2_nomecor` | brms | 89 | 8.7e-16 | 4 |
| `s2_mi_lognormal` | brms | 10 | 2.6e-16 | 2 |
| `s2_mi_trunc_lb` | brms | 10 | 4.4e-16 | 2 |
| `s2_mixture_theta` | brms | 9 | 5.6e-16 | 5 |
| `s2_mm` | brms | 10 | 0 (bitwise) | 0 |
| `s2_mm_weights` | brms | 10 | 0 (bitwise) | 0 |
| `s2_mmc` | brms | 17 | 1.1e-15 | 10 |
| `s2_mo_simo_prior` | brms | 6 | 0 (bitwise) | 0 |
| `s2_multinomial` | brms | 5 | 2.1e-16 | 1 |
| `s2_mv_shared_re` | brms | 20 | 1.9e-16 | 1 |
| `s2_mv_subset` | brms | 7 | 0 (bitwise) | 0 |
| `s2_nl_noloop` | brms | 6 | 0 (bitwise) | 0 |
| `s2_nlf` | brms | 4 | 1.3e-16 | 1 |
| `s2_rate` | brms | 3 | 2.9e-16 | 2 |
| `s2_s_by` | brms | 53 | 2.2e-16 | 8 |
| `s2_s_cc` | brms | 12 | 1.9e-16 | 1 |
| `s2_sar` | brms | 5 | 5.0e-16 | 4 |
| `s2_sar_error` | brms | 5 | 3.3e-16 | 2 |
| `s2_shifted_lognormal` | brms | 5 | 1.5e-16 | 1 |
| `s2_t2_by` | brms | 57 | 1.2e-16 | 1 |
| `s2_threading` | brms | 5 | 0 (bitwise) | 0 |
| `s2_unstr` | brms | 32 | 7.7e-16 | 10 |
| `s2_weights_trunc` | brms | 4 | 1.3e-16 | 1 |
| `s2_wiener` | brms | 6 | 2.6e-16 | 2 |
| `s2_zi_asymlaplace` | brms | 6 | 8.2e-16 | 4 |
| `s2_zi_beta` | brms | 5 | 3.6e-16 | 2 |
| `s2_zoi_beta` | brms | 6 | 3.8e-16 | 2 |
| `seeds_centered_model` | posteriordb | 27 | 0 (bitwise) | 0 |
| `seeds_model` | posteriordb | 27 | 0 (bitwise) | 0 |
| `seeds_stanified_model` | posteriordb | 27 | 0 (bitwise) | 0 |
| `sesame_one_pred_a` | posteriordb | 4 | 0 (bitwise) | 0 |
| `soil_incubation` | posteriordb | 7 | 1.6e-13 | 1241 |
| `state_space_stochastic_level_stochastic_seasonal` | posteriordb | 390 | 5.8e-16 | 12 |
| `sum_to_zero` | stanc3 | 148 | 0 (bitwise) | 0 |
| `surgical_model` | posteriordb | 15 | 0 (bitwise) | 0 |
| `sw_acat` | brms | 5 | 3.7e-14 | 196 |
| `sw_acat_cs` | brms | 8 | 4.8e-14 | 383 |
| `sw_ar` | brms | 5 | 6.2e-17 | 9 |
| `sw_arma` | brms | 6 | 4.3e-16 | 2 |
| `sw_asymlaplace` | brms | 5 | 7.5e-16 | 5 |
| `sw_bernoulli` | brms | 4 | 0 (bitwise) | 0 |
| `sw_beta` | brms | 4 | 1.7e-16 | 1 |
| `sw_binomial` | brms | 3 | 7.1e-16 | 4 |
| `sw_categorical` | brms | 5 | 0 (bitwise) | 0 |
| `sw_cens` | brms | 4 | 0 (bitwise) | 0 |
| `sw_cratio` | brms | 5 | 4.4e-13 | 3974 |
| `sw_cratio_cs` | brms | 8 | 2.8e-13 | 1528 |
| `sw_cumulative` | brms | 6 | 0 (bitwise) | 0 |
| `sw_cumulative_cs` | brms | 8 | 3.3e-13 | 5861 |
| `sw_dist_sigma` | brms | 5 | 4.4e-16 | 4 |
| `sw_exgaussian` | brms | 5 | 1.3e-16 | 1 |
| `sw_gamma` | brms | 4 | 2.8e-16 | 2 |
| `sw_gaussian` | brms | 5 | 0 (bitwise) | 0 |
| `sw_gp` | brms | 45 | 0 (bitwise) | 0 |
| `sw_hurdle_gamma` | brms | 5 | 2.8e-16 | 2 |
| `sw_hurdle_lognormal` | brms | 5 | 0 (bitwise) | 0 |
| `sw_hurdle_pois` | brms | 4 | 2.2e-16 | 1 |
| `sw_lognormal` | brms | 4 | 0 (bitwise) | 0 |
| `sw_ma` | brms | 5 | 1.2e-15 | 176 |
| `sw_me` | brms | 46 | 3.6e-16 | 3 |
| `sw_mi` | brms | 10 | 1.2e-16 | 1 |
| `sw_mixture` | brms | 8 | 4.2e-16 | 4 |
| `sw_mono` | brms | 6 | 1.7e-16 | 6 |
| `sw_mv_norescor` | brms | 7 | 0 (bitwise) | 0 |
| `sw_mv_rescor` | brms | 8 | 2.6e-16 | 2 |
| `sw_negbinomial` | brms | 5 | 0 (bitwise) | 0 |
| `sw_nonlinear` | brms | 4 | 5.6e-16 | 5 |
| `sw_poisson` | brms | 4 | 0 (bitwise) | 0 |
| `sw_re_bern` | brms | 9 | 0 (bitwise) | 0 |
| `sw_re_gauss` | brms | 10 | 0 (bitwise) | 0 |
| `sw_re_negbin` | brms | 10 | 0 (bitwise) | 0 |
| `sw_re_pois` | brms | 9 | 0 (bitwise) | 0 |
| `sw_re_slope` | brms | 17 | 2.1e-16 | 1 |
| `sw_se` | brms | 3 | 1.2e-16 | 1 |
| `sw_skewnormal` | brms | 5 | 9.4e-16 | 68 |
| `sw_spline_s` | brms | 13 | 1.9e-16 | 28 |
| `sw_spline_t2` | brms | 30 | 1.1e-16 | 1 |
| `sw_sratio` | brms | 5 | 4.4e-13 | 3974 |
| `sw_student` | brms | 6 | 0 (bitwise) | 0 |
| `sw_trunc` | brms | 4 | 1.8e-16 | 1 |
| `sw_vonmises` | brms | 4 | 9.8e-16 | 5 |
| `sw_weibull` | brms | 4 | 1.7e-15 | 120 |
| `sw_weights` | brms | 4 | 1.1e-15 | 19 |
| `sw_zi_binomial` | brms | 4 | 7.1e-16 | 4 |
| `sw_zi_negbin` | brms | 5 | 4.2e-16 | 2 |
| `sw_zi_poisson` | brms | 4 | 3.9e-16 | 2 |
| `tern_op_contains_var` | stanc3 | 3 | 0 (bitwise) | 0 |
| `validate_set_double_offset_multiplier_good` | stanc3 | 21 | 1.9e-16 | 1 |
| `vector-size-stmts` | stanc3 | 110 | 0 (bitwise) | 0 |
| `wells_daae_c_model` | posteriordb | 7 | 0 (bitwise) | 0 |
| `wells_dae_c_model` | posteriordb | 6 | 0 (bitwise) | 0 |
| `wells_dae_inter_model` | posteriordb | 8 | 0 (bitwise) | 0 |
| `wells_dae_model` | posteriordb | 5 | 0 (bitwise) | 0 |
| `wells_dist` | posteriordb | 3 | 0 (bitwise) | 0 |
| `wells_dist100_model` | posteriordb | 3 | 0 (bitwise) | 0 |
| `wells_dist100ars_model` | posteriordb | 4 | 0 (bitwise) | 0 |
| `wells_interaction_c_model` | posteriordb | 5 | 0 (bitwise) | 0 |
| `wells_interaction_model` | posteriordb | 5 | 0 (bitwise) | 0 |

Numerical notes:

- `dogs`: CmdStan sums Bernoulli terms one call per iteration; stanli's merged call uses Eigen reductions and vectorized exp/log1p. The different reduction order can change final rounding in the log density and gradient. Worst recorded deviation across all points: 32 ULP.
- `dogs_log`: As for dogs, merging Bernoulli terms changes their reduction order and can change final rounding. The primary point need not have the largest deviation of the three probes. Worst recorded deviation across all points: 25 ULP.

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
| `aalto_bern` | 12 |
| `aalto_binom` | 1 |
| `aalto_binom2` | 3 |
| `aalto_binomb` | 2 |
| `aalto_gpareto` | 30 |
| `aalto_grp_aov` | 4 |
| `aalto_grp_prior_mean` | 6 |
| `aalto_grp_prior_mean_var` | 10 |
| `aalto_lin` | 20 |
| `aalto_lin_std` | 29 |
| `aalto_lin_std_t` | 30 |
| `aalto_poisson_hurdle` | 1002 |
| `aalto_poisson_simple` | 1001 |
| `accel_gp` | 72 |
| `accel_splines` | 160 |
| `arK` | 7 |
| `arma11` | 4 |
| `blr` | 6 |
| `bones_model` | 13 |
| `bym2_offset_only` | 9610 |
| `ch09_m5_8s` | 4 |
| `ch09_m5_8s2` | 4 |
| `ch09_m9_1` | 5 |
| `ch09_m9_1_chains4` | 5 |
| `ch09_m9_2` | 2 |
| `ch09_m9_3` | 2 |
| `ch09_m9_4` | 3 |
| `ch09_m9_5` | 3 |
| `ch09_mp` | 2 |
| `ch11_m11_10` | 24 |
| `ch11_m11_11` | 25 |
| `ch11_m11_4` | 1019 |
| `ch11_m11_5` | 1019 |
| `ch11_m11_6` | 67 |
| `ch11_m11_7` | 2 |
| `ch11_m11_8` | 8 |
| `ch11_m11_9` | 12 |
| `ch11_m_pois` | 2 |
| `ch12_m12_1` | 4 |
| `ch12_m12_2` | 26 |
| `ch12_m12_3` | 2 |
| `ch12_m12_3_alt` | 2 |
| `ch12_m12_4` | 6 |
| `ch12_m12_5` | 11 |
| `ch12_m12_6` | 17 |
| `ch12_m12_7` | 10 |
| `ch13_m13_1` | 144 |
| `ch13_m13_2` | 146 |
| `ch13_m13_3` | 62 |
| `ch13_m13_4` | 1028 |
| `ch13_m13_4b` | 20 |
| `ch13_m13_4nc` | 33 |
| `ch13_m13_5` | 1021 |
| `ch13_m13_6` | 1029 |
| `ch13_m13_7` | 2 |
| `ch13_m13_7nc` | 3 |
| `ch14_m14_1` | 49 |
| `ch14_m14_10` | 4 |
| `ch14_m14_11` | 5 |
| `ch14_m14_2` | 96 |
| `ch14_m14_3` | 1188 |
| `ch14_m14_4` | 3 |
| `ch14_m14_4x` | 3 |
| `ch14_m14_5` | 4 |
| `ch14_m14_6` | 10 |
| `ch14_m14_6x` | 10 |
| `ch14_m14_7` | 1266 |
| `ch14_m14_8` | 15 |
| `ch14_m14_8nc` | 225 |
| `ch14_m14_9` | 4 |
| `ch15_m15_1` | 54 |
| `ch15_m15_2` | 104 |
| `ch15_m15_3` | 2 |
| `ch15_m15_4` | 2 |
| `ch15_m15_5` | 18 |
| `ch15_m15_6` | 6 |
| `ch15_m15_7` | 24 |
| `ch15_m15_8` | 3 |
| `ch15_m15_9` | 303 |
| `ch16_m16_1` | 3 |
| `ch16_m16_4` | 3 |
| `cholesky_cov_param_block` | 29 |
| `covid19imperial_v2` | 8457 |
| `covid19imperial_v3` | 8457 |
| `declare-define-multi` | 452 |
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
| `extra_hurdle_poisson` | 2 |
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
| `i319_gauss_re` | 127 |
| `i319_negbin_fixed` | 8 |
| `i319_negbin_re` | 127 |
| `i319_pois_fixed` | 7 |
| `i319_pois_re` | 126 |
| `i319_pois_re2` | 599 |
| `i320_gp_expquad` | 36 |
| `i320_gp_matern32` | 36 |
| `i320_mi_nhanes` | 30 |
| `i320_pois_trunc_both` | 6 |
| `i320_pois_trunc_ub` | 6 |
| `i320_sratio_cs` | 13 |
| `i320_sratio_plain` | 11 |
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
| `lupdf-inlining` | 4 |
| `mesquite` | 8 |
| `mother` | 899 |
| `multi_occupancy` | 312 |
| `multidim_var_param_ar45_mat23` | 120 |
| `nes` | 10 |
| `nes_logit_model` | 2 |
| `nn_rbm1bJ10` | 7951 |
| `nn_rbm1bJ100` | 79411 |
| `normal_mixture` | 3 |
| `normal_mixture_k` | 15 |
| `one_comp_mm_elim_abs` | 44 |
| `operators` | 35 |
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
| `reductions_allowed` | 251 |
| `s2_ar_cov` | 70 |
| `s2_beta_binomial` | 5 |
| `s2_car` | 12 |
| `s2_car_esicar` | 15 |
| `s2_car_icar` | 16 |
| `s2_categorical_re` | 29 |
| `s2_cens_interval` | 5 |
| `s2_com_poisson` | 5 |
| `s2_cosy` | 70 |
| `s2_cox` | 9 |
| `s2_cox_cens` | 9 |
| `s2_cumulative_cauchit` | 9 |
| `s2_cumulative_cloglog` | 9 |
| `s2_cumulative_probit` | 9 |
| `s2_custom_vint` | 5 |
| `s2_custom_vreal` | 5 |
| `s2_dirichlet` | 8 |
| `s2_discrete_weibull` | 5 |
| `s2_dist_sigma_re` | 17 |
| `s2_fcor` | 5 |
| `s2_frechet` | 5 |
| `s2_gev` | 6 |
| `s2_gp_approx` | 11 |
| `s2_gp_by_approx` | 18 |
| `s2_gp_by_gr` | 54 |
| `s2_gr_by` | 17 |
| `s2_gr_student` | 27 |
| `s2_hurdle_cumulative` | 8 |
| `s2_hurdle_negbin` | 6 |
| `s2_index_mi` | 12 |
| `s2_logistic_normal` | 18 |
| `s2_me2` | 259 |
| `s2_me2_nomecor` | 170 |
| `s2_mi_lognormal` | 12 |
| `s2_mi_trunc_lb` | 12 |
| `s2_mixture_theta` | 14 |
| `s2_mm` | 16 |
| `s2_mm_weights` | 16 |
| `s2_mmc` | 46 |
| `s2_mo_simo_prior` | 8 |
| `s2_multinomial` | 7 |
| `s2_mv_shared_re` | 50 |
| `s2_mv_subset` | 9 |
| `s2_nl_noloop` | 6 |
| `s2_nlf` | 4 |
| `s2_rate` | 4 |
| `s2_s_by` | 94 |
| `s2_s_cc` | 21 |
| `s2_sar` | 6 |
| `s2_sar_error` | 6 |
| `s2_shifted_lognormal` | 6 |
| `s2_t2_by` | 100 |
| `s2_threading` | 6 |
| `s2_unstr` | 161 |
| `s2_weights_trunc` | 5 |
| `s2_wiener` | 7 |
| `s2_zi_asymlaplace` | 7 |
| `s2_zi_beta` | 6 |
| `s2_zoi_beta` | 7 |
| `seeds_centered_model` | 47 |
| `seeds_model` | 27 |
| `seeds_stanified_model` | 26 |
| `sesame_one_pred_a` | 3 |
| `soil_incubation` | 31 |
| `state_space_stochastic_level_stochastic_seasonal` | 581 |
| `sum_to_zero` | 630 |
| `surgical_model` | 28 |
| `sw_acat` | 9 |
| `sw_acat_cs` | 12 |
| `sw_ar` | 6 |
| `sw_arma` | 7 |
| `sw_asymlaplace` | 6 |
| `sw_bernoulli` | 5 |
| `sw_beta` | 5 |
| `sw_binomial` | 4 |
| `sw_categorical` | 7 |
| `sw_cens` | 5 |
| `sw_cratio` | 9 |
| `sw_cratio_cs` | 12 |
| `sw_cumulative` | 10 |
| `sw_cumulative_cs` | 12 |
| `sw_dist_sigma` | 7 |
| `sw_exgaussian` | 6 |
| `sw_gamma` | 5 |
| `sw_gaussian` | 6 |
| `sw_gp` | 46 |
| `sw_hurdle_gamma` | 6 |
| `sw_hurdle_lognormal` | 6 |
| `sw_hurdle_pois` | 5 |
| `sw_lognormal` | 5 |
| `sw_ma` | 6 |
| `sw_me` | 87 |
| `sw_mi` | 12 |
| `sw_mixture` | 15 |
| `sw_mono` | 8 |
| `sw_mv_norescor` | 9 |
| `sw_mv_rescor` | 18 |
| `sw_negbinomial` | 6 |
| `sw_nonlinear` | 4 |
| `sw_poisson` | 5 |
| `sw_re_bern` | 15 |
| `sw_re_gauss` | 16 |
| `sw_re_negbin` | 16 |
| `sw_re_pois` | 15 |
| `sw_re_slope` | 46 |
| `sw_se` | 5 |
| `sw_skewnormal` | 6 |
| `sw_spline_s` | 22 |
| `sw_spline_t2` | 52 |
| `sw_sratio` | 9 |
| `sw_student` | 7 |
| `sw_trunc` | 5 |
| `sw_vonmises` | 5 |
| `sw_weibull` | 5 |
| `sw_weights` | 5 |
| `sw_zi_binomial` | 5 |
| `sw_zi_negbin` | 6 |
| `sw_zi_poisson` | 5 |
| `tern_op_contains_var` | 2 |
| `validate_set_double_offset_multiplier_good` | 22 |
| `vector-size-stmts` | 323 |
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

- `s2_invgaussian`
- `sir`

## Evaluate but not verified

- `kronecker_gp`: max rel diff 6.3e-03
  - lp matches CmdStan to 1e-13 and 436/438 gradients match; the two that flow through eigenvectors_sym differ by 0.7%. The covariance at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest 6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - lambda_j), so last-bit differences in the input are amplified by ~1e16. Every component op (eigen decomposition, transpose, matrix product, the whole chain with one operand held constant) matches CmdStan bitwise in isolation.

## Failures

- `sir`: EVAL_FAIL stanli MIR check: y is -1.82492e-07, but must be greater than or equal to 0.000000
