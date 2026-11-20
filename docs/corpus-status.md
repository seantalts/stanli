# Corpus status

Models that evaluate: 412/413
Models with CmdStan reference values: 412/413, at 1236 evaluation points in total.

The corpus includes models from posteriordb, generated brms and cogmod models, ctsem's state-space template, models from CRAN packages, imported teaching models, and language fixtures. The source collection column says where each model came from. `tools/verify_refs.py` checks all reference values the same way.

For 396 verified models, the table below shows how far stanli was from CmdStan at the primary evaluation point. These numbers were measured when the reference was recorded. Imported references keep their original CmdStan answers and a record of how each one was made. No deviation was recorded for them, so their rows say "not recorded". Having a reference for a model does not mean that the current build matches it. The numerical replay checks the current build.

A model passes only when tools/verify_sample.py finds that stanli's log_prob and full gradient match CmdStan's at a shared, fixed evaluation point. The accuracy columns show the worst deviation over lp and every gradient component, as a relative difference and in ULPs. 0 ULP means bitwise identical to CmdStan. The bitwise results are for information only. The replay requires a scaled error of at most 1e-9, with documented exceptions for ill-conditioned models. Models that evaluate but are not verified are listed in a separate section and do not count as passing.

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
| `beeguts_guts_sd` | beeguts | 5 | 3.6e-16 | 2 |
| `blr` | posteriordb | 7 | 1.8e-16 | 1 |
| `bmgarch_bekk_arma` | bmgarch | 28 | 1.5e-15 | 8 |
| `bmgarch_ccc` | bmgarch | 23 | 1.2e-15 | 40 |
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
| `cm_betadiscrete` | cogmod | 5 | 3.8e-16 | 2 |
| `cm_betagate` | cogmod | 6 | 6.6e-15 | 59 |
| `cm_bisa` | cogmod | 6 | 0 (bitwise) | 0 |
| `cm_choco` | cogmod | 10 | 2.6e-16 | 2 |
| `cm_ddm` | cogmod | 10 | 0 (bitwise) | 0 |
| `cm_exgaussian` | cogmod | 5 | 1.8e-16 | 1 |
| `cm_exwald` | cogmod | 7 | 0 (bitwise) | 0 |
| `cm_gamma` | cogmod | 6 | 0 (bitwise) | 0 |
| `cm_geg` | cogmod | 6 | 1.4e-16 | 1 |
| `cm_invgamma` | cogmod | 6 | 0 (bitwise) | 0 |
| `cm_invgaussian` | cogmod | 8 | 1.9e-16 | 28 |
| `cm_invweibull` | cogmod | 6 | 0 (bitwise) | 0 |
| `cm_lba1` | cogmod | 8 | 0 (bitwise) | 0 |
| `cm_lba2` | cogmod | 10 | 0 (bitwise) | 0 |
| `cm_lnr` | cogmod | 9 | 1.1e-16 | 2 |
| `cm_lnr_bench` | cogmod | 10 | 0 (bitwise) | 0 |
| `cm_loggamma` | cogmod | 7 | 0 (bitwise) | 0 |
| `cm_lognormal` | cogmod | 7 | 0 (bitwise) | 0 |
| `cm_logstudent` | cogmod | 7 | 0 (bitwise) | 0 |
| `cm_logweibull` | cogmod | 6 | 0 (bitwise) | 0 |
| `cm_rdm` | cogmod | 8 | 0 (bitwise) | 0 |
| `cm_weibull` | cogmod | 6 | 0 (bitwise) | 0 |
| `covid19imperial_v2` | posteriordb | 52 | 8.2e-16 | 7 |
| `covid19imperial_v3` | posteriordb | 52 | 8.2e-16 | 7 |
| `ctsem_ctsm` | ctsem | 182 | 2.0e-15 | 224 |
| `ctsem_ctsm_400` | ctsem | 1622 | 2.4e-14 | 192 |
| `ctsem_ctsm_post4_100` | ctsem | 581 | 1.1e-13 | 2976 |
| `diamonds` | posteriordb | 27 | 0 (bitwise) | 0 |
| `dogs` | posteriordb | 4 | 5.5e-15 | 31 |
| `dogs_hierarchical` | posteriordb | 3 | 1.2e-15 | 7 |
| `dogs_log` | posteriordb | 3 | 0 (bitwise) | 0 |
| `dogs_nonhierarchical` | posteriordb | 66 | 6.9e-16 | 4 |
| `dugongs_model` | posteriordb | 5 | 1.9e-16 | 1 |
| `earn_height` | posteriordb | 4 | 0 (bitwise) | 0 |
| `ecoensemble_hierarchical_withdrivers` | ecoensemble | 64 | 2.0e-14 | 304 |
| `ecoensemble_hierarchical_withdrivers_conj` | ecoensemble | 64 | 2.0e-14 | 416 |
| `eight_schools_centered` | posteriordb | 11 | 0 (bitwise) | 0 |
| `eight_schools_noncentered` | posteriordb | 11 | 0 (bitwise) | 0 |
| `election88_full` | posteriordb | 91 | 9.9e-15 | 82 |
| `empty_return_shape` | regressions | 4 | 0 (bitwise) | 0 |
| `epinow2_estimate_infections` | epinow2 | 25 | 4.2e-11 | 299015 |
| `epinow2_estimate_secondary` | epinow2 | 5 | 7.3e-15 | 64 |
| `examplemodels_arch1` | examplemodels | 4 | 0 (bitwise) | 0 |
| `examplemodels_cjs_cov_raneff` | examplemodels | 24 | 1.8e-15 | 14 |
| `examplemodels_cjs_mnl_ran` | examplemodels | 22 | 1.1e-15 | 6 |
| `examplemodels_covid_sens_spec` | examplemodels | 24 | 2.4e-16 | 2 |
| `examplemodels_dlm_fx_equicorr` | examplemodels | 8 | 2.3e-16 | 2 |
| `examplemodels_dlm_fx_factor` | examplemodels | 8 | 0 (bitwise) | 0 |
| `examplemodels_dlm_nile` | examplemodels | 3 | 0 (bitwise) | 0 |
| `examplemodels_gp_fit_multi_output` | examplemodels | 312 | 0 (bitwise) | 0 |
| `examplemodels_hmm_marginal` | examplemodels | 11 | 4.0e-16 | 3 |
| `examplemodels_irt_1pl_hier` | examplemodels | 34 | 0 (bitwise) | 0 |
| `examplemodels_irt_multilevel` | examplemodels | 56 | 1.2e-15 | 32 |
| `examplemodels_kidney` | examplemodels | 47 | 0 (bitwise) | 0 |
| `examplemodels_mr_ss` | examplemodels | 3 | 2.0e-13 | 1457 |
| `examplemodels_ms` | examplemodels | 7 | 3.3e-13 | 2629 |
| `examplemodels_ode_one_comp_mm` | examplemodels | 5 | 3.6e-15 | 256 |
| `examplemodels_probit_multi_good` | examplemodels | 2039 | 1.7e-15 | 4096 |
| `examplemodels_radon_group` | examplemodels | 93 | 9.4e-15 | 64 |
| `examplemodels_roaches_overdispersion` | examplemodels | 268 | 0 (bitwise) | 0 |
| `examplemodels_ssm` | examplemodels | 29 | 0 (bitwise) | 0 |
| `examplemodels_stagnant2` | examplemodels | 6 | 0 (bitwise) | 0 |
| `examplemodels_stochvol_optimized` | examplemodels | 504 | 0 (bitwise) | 0 |
| `examplemodels_sum_to_zero_binomial` | examplemodels | 20 | 1.4e-15 | 8 |
| `examplemodels_sur` | examplemodels | 28 | 2.5e-16 | 2 |
| `extra_hurdle_poisson` | rethinking | 3 | 2.2e-16 | 1 |
| `garch11` | posteriordb | 5 | 1.6e-15 | 8 |
| `gp_pois_regr` | posteriordb | 14 | 0 (bitwise) | 0 |
| `gp_regr` | posteriordb | 4 | 0 (bitwise) | 0 |
| `gpcm_latent_reg_irt` | posteriordb | 531 | 2.6e-13 | 4608 |
| `grsm_latent_reg_irt` | posteriordb | 409 | 1.2e-14 | 81 |
| `hbayesdm_dd_hyperbolic` | hbayesdm | 45 | 8.4e-16 | 7 |
| `hbayesdm_gng_m4` | hbayesdm | 73 | 5.6e-16 | 5 |
| `hbayesdm_igt_pvl_delta` | hbayesdm | 25 | 4.2e-16 | 2 |
| `hbayesdm_prl_rp` | hbayesdm | 67 | 8.6e-16 | 5 |
| `hdbayes_lm_npp` | hdbayes | 7 | 7.1e-16 | 4 |
| `hdbayes_pwe_npp` | hdbayes | 9 | 7.5e-16 | 4 |
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
| `mesquite` | posteriordb | 9 | 0 (bitwise) | 0 |
| `mlumr_survival_mspline_relaxed` | mlumr | 33 | 6.1e-16 | 22 |
| `mlumr_survival_relaxed` | mlumr | 13 | 1.6e-16 | 1 |
| `multi_occupancy` | posteriordb | 107 | 4.8e-15 | 36 |
| `multinma_ndmm_mspline` | multinma | 63 | 4.4e-16 | 16 |
| `multinma_smoking_re` | multinma | 60 | 4.8e-16 | 4 |
| `nes` | posteriordb | 11 | 0 (bitwise) | 0 |
| `nes_logit_model` | posteriordb | 3 | 0 (bitwise) | 0 |
| `nn_rbm1bJ10` | posteriordb | 7952 | 4.6e-16 | 3 |
| `nn_rbm1bJ100` | posteriordb | 79412 | 1.1e-13 | 735 |
| `normal_mixture` | posteriordb | 4 | 0 (bitwise) | 0 |
| `normal_mixture_k` | posteriordb | 15 | 1.2e-13 | 1086 |
| `oncobayes2_blrm_exnex_combo2` | oncobayes2 | 53 | 0 (bitwise) | 0 |
| `oncobayes2_blrm_exnex_combo3` | oncobayes2 | 100 | 3.8e-15 | 94 |
| `oncobayes2_blrm_exnex_sa` | oncobayes2 | 10 | 0 (bitwise) | 0 |
| `one_comp_mm_elim_abs` | posteriordb | 5 | 6.9e-15 | 256 |
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
| `rbioacc_odetk` | rbioacc | 4 | 3.5e-16 | 2 |
| `rbmi_mmrm` | regressions | 5 | 2.2e-16 | 1 |
| `reject_udf_data_size` | regressions | 2 | 0 (bitwise) | 0 |
| `rstanarm_glm_gaussian` | rstanarm | 5 | 5.5e-16 | 4 |
| `rstanarm_glmer_bernoulli` | rstanarm | 72 | 8.9e-16 | 16 |
| `rstanarm_glmer_nb_count` | rstanarm | 26 | 4.3e-16 | 3 |
| `rstanarm_polr` | rstanarm | 10 | 2.1e-15 | 19 |
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
| `stanguide_changepoint` | stanguide | 3 | 0 (bitwise) | 0 |
| `stanguide_cjs_history` | stanguide | 6 | 8.4e-16 | 4 |
| `stanguide_copula` | stanguide | 7 | 0 (bitwise) | 0 |
| `stanguide_integrate_1d_normal` | stanguide | 4 | 0 (bitwise) | 0 |
| `stanguide_map_rect_logistic` | stanguide | 3 | 0 (bitwise) | 0 |
| `state_space_stochastic_level_stochastic_seasonal` | posteriordb | 390 | 5.8e-16 | 12 |
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
| `tuple_array_params` | regressions | 55 | 1.6e-16 | 1 |
| `tuple_coef_cont` | regressions | 41 | 2.2e-16 | 4 |
| `tuple_decompositions` | regressions | 10 | 5.6e-17 | 1 |
| `tuple_decompositions_more` | regressions | 4 | 6.9e-18 | 4 |
| `tuple_effect_once` | regressions | 2 | 0 (bitwise) | 0 |
| `tuple_loops` | regressions | 7 | 4.2e-16 | 2 |
| `tuple_reduce_sum` | regressions | 5 | 0 (bitwise) | 0 |
| `tuple_return_slices` | regressions | 5 | 1.8e-16 | 1 |
| `walker_rw1_model` | walker | 5 | 8.4e-16 | 5 |
| `walker_rw1_model_naive` | walker | 305 | 0 (bitwise) | 0 |
| `wells_daae_c_model` | posteriordb | 7 | 0 (bitwise) | 0 |
| `wells_dae_c_model` | posteriordb | 6 | 0 (bitwise) | 0 |
| `wells_dae_inter_model` | posteriordb | 8 | 0 (bitwise) | 0 |
| `wells_dae_model` | posteriordb | 5 | 0 (bitwise) | 0 |
| `wells_dist` | posteriordb | 3 | 0 (bitwise) | 0 |
| `wells_dist100_model` | posteriordb | 3 | 0 (bitwise) | 0 |
| `wells_dist100ars_model` | posteriordb | 4 | 0 (bitwise) | 0 |
| `wells_interaction_c_model` | posteriordb | 5 | 0 (bitwise) | 0 |
| `wells_interaction_model` | posteriordb | 5 | 0 (bitwise) | 0 |
| `while_counter_size` | regressions | 8 | 0 (bitwise) | 0 |
| `while_udf_slices` | regressions | 7 | 1.4e-16 | 1 |

Numerical notes:

- `dogs`: CmdStan sums Bernoulli terms one call per iteration; stanli's merged call uses Eigen reductions and vectorized exp/log1p. The different reduction order can change final rounding in the log density and gradient. Worst recorded deviation across all points: 32 ULP.
- `dogs_log`: As for dogs, merging Bernoulli terms changes their reduction order and can change final rounding. The primary point need not have the largest deviation of the three probes. Worst recorded deviation across all points: 25 ULP.
- `tuple_array_params`: At the third point the gradient of grid[1, 2].1's free value is 1 - x - x^2 * sum(v^2) / 9 with x = 1 and v = (1/3, 1/3, 1/3), which cancels to -1/27. Its terms are summed in a different order: evaluated exactly at the same inputs, stanli is 8 ulp from the result and CmdStan 24 ulp, 32 ulp apart. Worst recorded deviation across all points: 32 ULP.

## write_array references

The reference recorder also saves CmdStan's write_array output at the same point. This covers every CSV column: constrained parameters, transformed parameters and generated quantities. Both drivers call write_array directly and use Stan's RNG with the same seed and chain 0, so the generated-quantity draws can be compared too. In CI, tools/verify_refs.py replays these rows. Column names must match exactly, and values must pass the same tolerance as the model's log density and gradient.

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
| `beeguts_guts_sd` | 540 |
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
| `cm_betadiscrete` | 8 |
| `cm_betagate` | 10 |
| `cm_bisa` | 10 |
| `cm_choco` | 18 |
| `cm_ddm` | 18 |
| `cm_exgaussian` | 8 |
| `cm_exwald` | 12 |
| `cm_gamma` | 10 |
| `cm_geg` | 10 |
| `cm_invgamma` | 10 |
| `cm_invgaussian` | 14 |
| `cm_invweibull` | 10 |
| `cm_lba1` | 14 |
| `cm_lba2` | 18 |
| `cm_lnr` | 16 |
| `cm_lnr_bench` | 16 |
| `cm_loggamma` | 12 |
| `cm_lognormal` | 12 |
| `cm_logstudent` | 12 |
| `cm_logweibull` | 10 |
| `cm_rdm` | 14 |
| `cm_weibull` | 10 |
| `covid19imperial_v2` | 8457 |
| `covid19imperial_v3` | 8457 |
| `ctsem_ctsm` | 648 |
| `ctsem_ctsm_400` | 4968 |
| `ctsem_ctsm_post4_100` | 4929 |
| `diamonds` | 27 |
| `dogs` | 2253 |
| `dogs_hierarchical` | 752 |
| `dogs_log` | 2252 |
| `dogs_nonhierarchical` | 946 |
| `dugongs_model` | 6 |
| `earn_height` | 3 |
| `ecoensemble_hierarchical_withdrivers` | 612 |
| `ecoensemble_hierarchical_withdrivers_conj` | 612 |
| `eight_schools_centered` | 10 |
| `eight_schools_noncentered` | 18 |
| `election88_full` | 11656 |
| `empty_return_shape` | 4 |
| `epinow2_estimate_infections` | 352 |
| `epinow2_estimate_secondary` | 72 |
| `examplemodels_arch1` | 3 |
| `examplemodels_cjs_cov_raneff` | 16574 |
| `examplemodels_covid_sens_spec` | 41 |
| `examplemodels_dlm_fx_equicorr` | 25 |
| `examplemodels_dlm_fx_factor` | 10 |
| `examplemodels_dlm_nile` | 2 |
| `examplemodels_gp_fit_multi_output` | 326 |
| `examplemodels_irt_1pl_hier` | 33 |
| `examplemodels_irt_multilevel` | 55 |
| `examplemodels_kidney` | 51 |
| `examplemodels_mr_ss` | 19602 |
| `examplemodels_ms` | 72036 |
| `examplemodels_ode_one_comp_mm` | 44 |
| `examplemodels_probit_multi_good` | 2064 |
| `examplemodels_radon_group` | 92 |
| `examplemodels_roaches_overdispersion` | 267 |
| `examplemodels_ssm` | 55 |
| `examplemodels_stagnant2` | 5 |
| `examplemodels_stochvol_optimized` | 1003 |
| `examplemodels_sum_to_zero_binomial` | 833 |
| `examplemodels_sur` | 33 |
| `extra_hurdle_poisson` | 2 |
| `garch11` | 4 |
| `gp_pois_regr` | 24 |
| `gp_regr` | 3 |
| `gpcm_latent_reg_irt` | 550 |
| `grsm_latent_reg_irt` | 418 |
| `hbayesdm_dd_hyperbolic` | 2266 |
| `hbayesdm_gng_m4` | 14548 |
| `hbayesdm_igt_pvl_delta` | 448 |
| `hbayesdm_prl_rp` | 8149 |
| `hdbayes_lm_npp` | 6 |
| `hdbayes_pwe_npp` | 64 |
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
| `mesquite` | 8 |
| `mlumr_survival_mspline_relaxed` | 1052 |
| `mlumr_survival_relaxed` | 925 |
| `multi_occupancy` | 312 |
| `multinma_ndmm_mspline` | 956 |
| `multinma_smoking_re` | 375 |
| `nes` | 10 |
| `nes_logit_model` | 2 |
| `nn_rbm1bJ10` | 7951 |
| `nn_rbm1bJ100` | 79411 |
| `normal_mixture` | 3 |
| `normal_mixture_k` | 15 |
| `oncobayes2_blrm_exnex_combo2` | 143 |
| `oncobayes2_blrm_exnex_combo3` | 271 |
| `oncobayes2_blrm_exnex_sa` | 23 |
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
| `rbioacc_odetk` | 124 |
| `rbmi_mmrm` | 15 |
| `reject_udf_data_size` | 1 |
| `rstanarm_glm_gaussian` | 9 |
| `rstanarm_glmer_bernoulli` | 142 |
| `rstanarm_glmer_nb_count` | 51 |
| `rstanarm_polr` | 113 |
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
| `stanguide_changepoint` | 114 |
| `stanguide_cjs_history` | 8 |
| `stanguide_copula` | 21 |
| `stanguide_integrate_1d_normal` | 3 |
| `stanguide_map_rect_logistic` | 2 |
| `stanguide_ode_adjoint` | 5 |
| `state_space_stochastic_level_stochastic_seasonal` | 581 |
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
| `tuple_array_params` | 109 |
| `tuple_coef_cont` | 49 |
| `tuple_decompositions` | 30 |
| `tuple_decompositions_more` | 68 |
| `tuple_effect_once` | 4 |
| `tuple_loops` | 96 |
| `tuple_reduce_sum` | 5 |
| `tuple_return_slices` | 100 |
| `walker_rw1_model` | 410 |
| `walker_rw1_model_naive` | 607 |
| `wells_daae_c_model` | 6 |
| `wells_dae_c_model` | 5 |
| `wells_dae_inter_model` | 7 |
| `wells_dae_model` | 4 |
| `wells_dist` | 2 |
| `wells_dist100_model` | 2 |
| `wells_dist100ars_model` | 3 |
| `wells_interaction_c_model` | 4 |
| `wells_interaction_model` | 4 |
| `while_counter_size` | 8 |
| `while_udf_slices` | 6 |

## Rejected by both engines

CmdStan and stanli both reject every shared evaluation point for these models. The model is invalid at those points, for example when an ODE solution dips below a declared lower bound. There is nothing to compare. The two engines agree, but these models do not count as verified.

- `s2_invgaussian`
- `sir`

## Evaluate but not verified

- `kronecker_gp`: max rel diff 6.3e-03
  - lp matches CmdStan to 1e-13 and 436/438 gradients match; the two that flow through eigenvectors_sym differ by 0.7%. The covariance at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest 6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - lambda_j), so last-bit differences in the input are amplified by ~1e16. Every component op (eigen decomposition, transpose, matrix product, the whole chain with one operand held constant) matches CmdStan bitwise in isolation.
- `stanguide_ode_adjoint`: max rel diff 3.9e-06
  - With the guide's adjoint call and tolerances of 1e-6, stanli and CmdStan differ by 3.9e-6 and 3.3e-6 relative at the first two points and agree to 3.8e-14 at the third. With every tolerance at 1e-11 the two agree to 5e-11 and 9e-11 at the first two points. Against that tight-tolerance solution at the first point, the log density is off by 3.0e-6 in both engines, the first gradient entry by 4.0e-6 in stanli and 1.5e-5 in CmdStan, and the second by 4.0e-5 in stanli and 4.4e-5 in CmdStan (absolute differences).

## Failures

- `sir`: EVAL_FAIL y[6][4] is -1.82492e-07, but must be greater than or equal to 0.000000
