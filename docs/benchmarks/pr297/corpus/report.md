# Runtime before/after benchmark

Before: `917c634170420071f76fb6fb90a5c47fd708c7ab`. After: `8d8b37f79dd9b5c5e408a33321664fd86a5d4f36`.

All ratios are after / before; lower is faster. Timings are medians; raw paired samples and quartiles are in results.json. Numerical ULPs cover LP and every gradient at all requested points; failures are not dropped.

| Model | Before grad (µs) | After grad (µs) | Ratio | Before prep (ms) | After prep (ms) | Before RSS (MiB) | After RSS (MiB) | Max ULP | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `2pl_latent_reg_irt` | 83.956 | 84.204 | 1.003 | 6.279 | 6.389 | 8.094 | 8.125 | 0 | measured |
| `GLMM1_model` | 10.455 | 10.544 | 1.008 | 3.516 | 3.487 | 7.109 | 7.281 | 0 | measured |
| `GLMM_Poisson_model` | 0.704 | 0.701 | 0.998 | 0.979 | 0.983 | 4.969 | 5.016 | 0 | measured |
| `GLM_Binomial_model` | 0.848 | 0.848 | 0.995 | 1.147 | 1.156 | 4.500 | 4.562 | 0 | measured |
| `GLM_Poisson_model` | 0.399 | 0.397 | 0.994 | 0.705 | 0.720 | 4.359 | 4.469 | 0 | measured |
| `M0_model` | 4.505 | 4.470 | 0.994 | 1.942 | 1.828 | 5.156 | 5.312 | 0 | measured |
| `Mb_model` | 41.924 | 41.770 | 0.997 | 17.031 | 17.071 | 13.203 | 13.297 | 0 | measured |
| `Mh_model` | 15.213 | 15.351 | 1.009 | 8.531 | 8.644 | 7.500 | 7.641 | 0 | measured |
| `Mt_model` | 4.567 | 4.523 | 0.990 | 2.009 | 1.934 | 5.297 | 5.469 | 0 | measured |
| `Mtbh_model` | 12.960 | 12.923 | 0.997 | 16.075 | 16.008 | 9.141 | 9.250 | 0 | measured |
| `Mth_model` | 24.050 | 23.701 | 0.981 | 20.006 | 19.848 | 8.000 | 8.078 | 0 | measured |
| `Rate_1_model` | 0.068 | 0.068 | 1.003 | 0.316 | 0.321 | 3.656 | 3.734 | 0 | measured |
| `Rate_2_model` | 0.125 | 0.126 | 1.006 | 0.535 | 0.535 | 4.094 | 4.172 | 0 | measured |
| `Rate_3_model` | 0.086 | 0.087 | 1.002 | 0.365 | 0.367 | 3.781 | 3.859 | 0 | measured |
| `Rate_4_model` | 0.101 | 0.101 | 0.999 | 0.470 | 0.446 | 3.922 | 3.984 | 0 | measured |
| `Rate_5_model` | 0.086 | 0.087 | 1.013 | 0.412 | 0.431 | 3.859 | 3.953 | 0 | measured |
| `Survey_model` | 54.327 | 54.827 | 1.005 | 5.030 | 4.737 | 6.672 | 6.812 | 0 | measured |
| `accel_gp` | 5.748 | 6.330 | 1.096 | 5.439 | 5.318 | 9.828 | 10.094 | 0 | >5% slower; confirm |
| `accel_splines` | 6.373 | 6.929 | 1.090 | 4.008 | 4.001 | 8.062 | 8.234 | 0 | >5% slower; confirm |
| `arK` | 1.782 | 1.782 | 1.002 | 2.027 | 1.968 | 6.406 | 6.625 | 0 | measured |
| `arma11` | 4.461 | 4.423 | 0.992 | 1.986 | 1.935 | 6.250 | 6.359 | 0 | measured |
| `blr` | 0.590 | 0.583 | 0.990 | 0.643 | 0.642 | 4.344 | 4.469 | 0 | measured |
| `bones_model` | 43.711 | 42.710 | 0.974 | 9.366 | 9.150 | 12.000 | 12.047 | 0 | measured |
| `bym2_offset_only` | 40.723 | 40.254 | 0.988 | 2.334 | 2.182 | 6.594 | 6.703 | 0 | measured |
| `covid19imperial_v2` | 233.843 | 241.318 | 1.029 | 2783.027 | 2793.622 | 265.250 | 268.969 | 0 | measured |
| `covid19imperial_v3` | 232.216 | 237.897 | 1.029 | 2766.343 | 2764.998 | 265.125 | 264.938 | 0 | measured |
| `diamonds` | 31.256 | 31.300 | 1.001 | 21.075 | 21.071 | 22.672 | 22.781 | 0 | measured |
| `dogs` | 7.348 | 7.263 | 0.992 | 17.185 | 18.099 | 31.875 | 31.938 | 0 | measured |
| `dogs_hierarchical` | 10.348 | 10.261 | 0.997 | 8.761 | 8.823 | 7.969 | 8.125 | 0 | measured |
| `dogs_log` | 7.927 | 7.925 | 1.002 | 17.248 | 18.191 | 31.891 | 31.891 | 0 | measured |
| `dogs_nonhierarchical` | 19.681 | 19.729 | 1.002 | 24.634 | 24.652 | 10.516 | 10.562 | 0 | measured |
| `dugongs_model` | 0.563 | 0.556 | 1.011 | 0.709 | 0.738 | 4.578 | 4.609 | 0 | measured |
| `earn_height` | 4.154 | 4.208 | 1.019 | 0.682 | 0.690 | 4.453 | 4.547 | 0 | measured |
| `eight_schools_centered` | 0.295 | 0.296 | 1.014 | 0.437 | 0.454 | 3.875 | 3.984 | 0 | measured |
| `eight_schools_noncentered` | 0.260 | 0.256 | 0.991 | 0.542 | 0.530 | 4.078 | 4.172 | 0 | measured |
| `election88_full` | 257.042 | 258.371 | 1.005 | 141.133 | 148.012 | 86.531 | 86.672 | 0 | measured |
| `garch11` | 6.949 | 7.962 | 1.145 | 2.118 | 2.131 | 6.203 | 6.375 | 0 | >5% slower; confirm |
| `gp_pois_regr` | 3.916 | 3.925 | 1.002 | 0.699 | 0.704 | 4.750 | 4.891 | 0 | measured |
| `gp_regr` | 4.290 | 4.233 | 0.999 | 0.635 | 0.634 | 4.672 | 4.703 | 0 | measured |
| `gpcm_latent_reg_irt` | 122.988 | 122.902 | 1.000 | 53.690 | 53.736 | 39.891 | 39.609 | 0 | measured |
| `grsm_latent_reg_irt` | 71.636 | 70.996 | 0.990 | 20.247 | 20.301 | 24.812 | 24.641 | 0 | measured |
| `hier_2pl` | 284.086 | 284.416 | 1.001 | 5.806 | 5.794 | 12.281 | 12.234 | 0 | measured |
| `hierarchical_gp` | 30.876 | 30.809 | 1.000 | 52.983 | 53.220 | 12.766 | 12.844 | 0 | measured |
| `hmm_drive_0` | 105.427 | 122.696 | 1.162 | 53.292 | 54.191 | 19.281 | 20.266 | 0 | >5% slower; confirm |
| `hmm_drive_1` | 112.042 | 128.177 | 1.151 | 53.617 | 54.950 | 19.484 | 20.438 | 0 | >5% slower; confirm |
| `hmm_example` | 16.357 | 19.261 | 1.165 | 11.569 | 11.720 | 8.484 | 9.094 | 0 | >5% slower; confirm |
| `hmm_gaussian` | 170.280 | 192.594 | 1.138 | 254.257 | 256.941 | 61.000 | 65.719 | 0 | >5% slower; confirm |
| `iohmm_reg` | 162.172 | 187.455 | 1.156 | 357.295 | 359.800 | 71.141 | 69.406 | 0 | >5% slower; confirm |
| `irt_2pl` | 16.627 | 16.698 | 1.008 | 1.186 | 1.190 | 5.641 | 5.641 | 0 | measured |
| `kidscore_interaction` | 2.495 | 2.511 | 1.006 | 0.726 | 0.739 | 4.484 | 4.562 | 0 | measured |
| `kidscore_interaction_c` | 2.514 | 2.483 | 0.992 | 0.740 | 0.736 | 4.516 | 4.578 | 0 | measured |
| `kidscore_interaction_c2` | 2.530 | 2.496 | 0.984 | 0.740 | 0.728 | 4.500 | 4.547 | 0 | measured |
| `kidscore_interaction_z` | 2.523 | 2.486 | 0.988 | 0.752 | 0.750 | 4.547 | 4.594 | 0 | measured |
| `kidscore_mom_work` | 2.487 | 2.499 | 1.004 | 1.529 | 1.537 | 4.469 | 4.531 | 0 | measured |
| `kidscore_momhs` | 1.547 | 1.549 | 1.001 | 0.640 | 0.628 | 4.250 | 4.359 | 0 | measured |
| `kidscore_momhsiq` | 2.019 | 2.071 | 1.020 | 0.693 | 0.704 | 4.406 | 4.547 | 0 | measured |
| `kidscore_momiq` | 1.564 | 1.555 | 1.001 | 0.624 | 0.636 | 4.219 | 4.344 | 0 | measured |
| `kilpisjarvi` | 0.331 | 0.334 | 1.007 | 0.525 | 0.536 | 4.188 | 4.266 | 0 | measured |
| `kronecker_gp` | 192.298 | 185.285 | 0.964 | 6.322 | 6.268 | 8.656 | 8.766 | 0 | measured |
| `ldaK2` | 48.910 | 49.560 | 1.015 | 6.425 | 6.425 | 10.281 | 10.344 | 0 | measured |
| `ldaK5` | 2370.463 | 2382.054 | 1.005 | 332.184 | 331.925 | 295.688 | 294.781 | 0 | measured |
| `log10earn_height` | 4.168 | 4.235 | 1.010 | 1.650 | 1.676 | 4.531 | 4.625 | 0 | measured |
| `logearn_height` | 4.177 | 4.173 | 1.000 | 0.706 | 0.718 | 4.453 | 4.531 | 0 | measured |
| `logearn_height_male` | 5.753 | 5.765 | 0.996 | 0.775 | 0.780 | 4.688 | 4.812 | 0 | measured |
| `logearn_interaction` | 7.413 | 7.417 | 1.001 | 0.816 | 0.821 | 4.844 | 4.875 | 0 | measured |
| `logearn_interaction_z` | 7.447 | 7.429 | 0.994 | 0.841 | 0.846 | 4.938 | 4.984 | 0 | measured |
| `logearn_logheight_male` | 5.673 | 5.763 | 1.000 | 0.791 | 0.806 | 4.750 | 4.781 | 0 | measured |
| `logistic_regression_rhs` | 40.636 | 40.906 | 1.006 | 11.634 | 11.727 | 16.953 | 16.984 | 0 | measured |
| `logmesquite` | 0.482 | 0.483 | 1.002 | 0.871 | 0.855 | 4.781 | 4.859 | 0 | measured |
| `logmesquite_logva` | 0.348 | 0.350 | 1.009 | 0.699 | 0.701 | 4.406 | 4.484 | 0 | measured |
| `logmesquite_logvas` | 0.485 | 0.482 | 0.993 | 0.869 | 0.878 | 4.797 | 4.859 | 0 | measured |
| `logmesquite_logvash` | 0.436 | 0.436 | 1.003 | 0.807 | 0.812 | 4.750 | 4.797 | 0 | measured |
| `logmesquite_logvolume` | 0.261 | 0.265 | 1.013 | 0.594 | 0.601 | 4.281 | 4.359 | 0 | measured |
| `losscurve_sislob` | 1.389 | 1.400 | 1.007 | 5.415 | 5.371 | 7.562 | 7.672 | 0 | measured |
| `lotka_volterra` | 21.595 | 26.509 | 1.238 | 1.356 | 1.352 | 5.656 | 5.719 | 0 | >5% slower; confirm |
| `low_dim_gauss_mix` | 48.336 | 48.432 | 1.002 | 3.261 | 3.372 | 6.875 | 7.047 | 0 | measured |
| `low_dim_gauss_mix_collapse` | 46.592 | 46.410 | 0.996 | 3.246 | 3.444 | 6.734 | 6.953 | 0 | measured |
| `lsat_model` | 38.118 | 38.048 | 0.999 | 3.485 | 3.552 | 5.250 | 5.266 | 0 | measured |
| `mesquite` | 0.486 | 0.485 | 1.003 | 0.767 | 0.782 | 4.594 | 4.703 | 0 | measured |
| `multi_occupancy` | 23.883 | 24.421 | 1.024 | 8.554 | 8.322 | 9.984 | 10.062 | 0 | measured |
| `nes` | 16.002 | 16.179 | 1.012 | 4.044 | 4.016 | 6.188 | 6.203 | 0 | measured |
| `nes_logit_model` | 6.186 | 6.168 | 0.995 | 0.588 | 0.594 | 4.406 | 4.375 | 0 | measured |
| `nn_rbm1bJ10` | 172.421 | 175.284 | 1.015 | 8.774 | 8.975 | 16.578 | 18.609 | 0 | measured |
| `nn_rbm1bJ100` | 414645.639 | 418918.139 | 1.010 | 2855.654 | 2889.800 | 4919.422 | 5274.312 | 0 | measured |
| `normal_mixture` | 42.160 | 42.461 | 1.000 | 3.117 | 3.129 | 7.438 | 7.547 | 0 | measured |
| `normal_mixture_k` | 179.136 | 189.103 | 1.044 | 28.498 | 28.530 | 34.047 | 33.766 | 0 | measured |
| `one_comp_mm_elim_abs` | 489.384 | 486.284 | 0.994 | 1.161 | 1.180 | 6.078 | 6.125 | 0 | measured |
| `pilots` | 0.773 | 0.777 | 1.002 | 1.089 | 1.094 | 5.172 | 5.234 | 0 | measured |
| `prophet` | 54.665 | 54.653 | 1.000 | 9.320 | 9.243 | 11.922 | 12.047 | 0 | measured |
| `radon_county` | 73.977 | 73.996 | 1.000 | 11.634 | 11.560 | 14.625 | 14.719 | 0 | measured |
| `radon_county_intercept` | 81.653 | 82.362 | 1.008 | 28.918 | 29.585 | 26.969 | 26.484 | 0 | measured |
| `radon_hierarchical_intercept_centered` | 98.021 | 97.970 | 0.999 | 47.423 | 48.428 | 48.109 | 48.094 | 0 | measured |
| `radon_hierarchical_intercept_noncentered` | 98.459 | 98.444 | 1.000 | 47.549 | 48.658 | 47.812 | 47.828 | 0 | measured |
| `radon_partially_pooled_centered` | 67.026 | 66.986 | 0.999 | 22.823 | 22.953 | 22.016 | 22.016 | 0 | measured |
| `radon_partially_pooled_noncentered` | 67.574 | 67.538 | 0.999 | 23.054 | 22.964 | 22.094 | 22.094 | 0 | measured |
| `radon_pooled` | 45.376 | 45.234 | 0.998 | 16.234 | 16.728 | 15.453 | 15.516 | 0 | measured |
| `radon_variable_intercept_centered` | 82.436 | 82.412 | 1.001 | 29.026 | 30.020 | 26.562 | 26.672 | 0 | measured |
| `radon_variable_intercept_noncentered` | 82.533 | 82.577 | 1.001 | 29.326 | 29.883 | 26.609 | 26.359 | 0 | measured |
| `radon_variable_intercept_slope_centered` | 120.453 | 120.484 | 1.000 | 32.745 | 33.392 | 31.656 | 31.797 | 0 | measured |
| `radon_variable_intercept_slope_noncentered` | 121.344 | 121.394 | 1.000 | 32.979 | 33.574 | 31.688 | 31.812 | 0 | measured |
| `radon_variable_slope_centered` | 83.909 | 83.986 | 1.000 | 29.041 | 29.813 | 26.641 | 26.656 | 0 | measured |
| `radon_variable_slope_noncentered` | 84.054 | 84.148 | 1.001 | 29.105 | 29.871 | 26.562 | 26.703 | 0 | measured |
| `rats_model` | 1.150 | 1.141 | 0.990 | 1.269 | 1.304 | 5.828 | 6.094 | 0 | measured |
| `seeds_centered_model` | 0.782 | 0.788 | 1.010 | 0.845 | 0.883 | 4.859 | 4.891 | 0 | measured |
| `seeds_model` | 0.767 | 0.775 | 1.010 | 0.842 | 0.843 | 4.859 | 4.906 | 0 | measured |
| `seeds_stanified_model` | 0.759 | 0.765 | 1.006 | 0.767 | 0.777 | 4.672 | 4.719 | 0 | measured |
| `sesame_one_pred_a` | 0.853 | 0.844 | 0.992 | 0.516 | 0.539 | 4.047 | 4.141 | 0 | measured |
| `sir` | — | — | — | — | — | — | — | — | no_joint_finite_point |
| `soil_incubation` | 28.091 | 36.499 | 1.304 | 1.657 | 1.646 | 6.391 | 6.469 | 0 | >5% slower; confirm |
| `state_space_stochastic_level_stochastic_seasonal` | 6.846 | 6.782 | 0.993 | 2.227 | 2.270 | 6.500 | 6.641 | 0 | measured |
| `surgical_model` | 0.577 | 0.579 | 1.003 | 0.662 | 0.675 | 4.328 | 4.469 | 0 | measured |
| `wells_daae_c_model` | 19.397 | 19.395 | 1.003 | 1.850 | 1.851 | 6.219 | 6.188 | 0 | measured |
| `wells_dae_c_model` | 17.882 | 17.763 | 0.993 | 1.808 | 1.786 | 5.906 | 5.875 | 0 | measured |
| `wells_dae_inter_model` | 19.842 | 19.703 | 0.993 | 1.864 | 1.926 | 6.250 | 6.234 | 0 | measured |
| `wells_dae_model` | 18.948 | 18.922 | 1.000 | 1.729 | 1.723 | 5.688 | 5.672 | 0 | measured |
| `wells_dist` | 21.141 | 21.152 | 1.005 | 1.521 | 1.548 | 5.250 | 5.297 | 0 | measured |
| `wells_dist100_model` | 15.652 | 16.021 | 1.002 | 1.632 | 1.608 | 5.484 | 5.484 | 0 | measured |
| `wells_dist100ars_model` | 17.297 | 17.292 | 1.002 | 1.649 | 1.670 | 5.500 | 5.531 | 0 | measured |
| `wells_interaction_c_model` | 18.935 | 18.943 | 1.000 | 1.734 | 1.718 | 5.688 | 5.641 | 0 | measured |
| `wells_interaction_model` | 19.003 | 18.951 | 0.997 | 1.688 | 1.699 | 5.594 | 5.609 | 0 | measured |
