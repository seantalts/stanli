# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling, it reuses a prepared autodiff graph and batches
independent work. This can shorten the first fit and repeated gradient
evaluations; the gain depends on the model.

The current comparison covers **<!--gen:benchmark_models-->319<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->315<!--/gen--> accepted gradient
comparisons, the median paired speedup is **<!--gen:corpus_median-->1.72x<!--/gen-->**.
The tables cover every model, with incomplete results listed separately.

<a id="benchmark-method"></a>

## How we measure

**Gradient speedup is CmdStan time / Stanli time:** above 1x favors Stanli.
Both engines evaluate the same log density and full gradient at a shared
unconstrained point. After warmup, six alternating pairs give a median
speedup. Every accepted pair must pass its numerical comparison.

**Total sampling is estimated as setup + 2,000 × median warm gradient time.**
Stanli setup includes source-to-MIR compilation and lowering/binding; CmdStan
setup includes stanc translation and the ordinary C++ model build. This
fixed-work estimate excludes sampler/output overhead. Full sampling is not run
in this benchmark.

See the [appendix](#appendix) and independent
[numerical checks](../TESTING.md#comparison-with-cmdstan-on-complete-models).

## Why some models are faster, and some slower

**Less setup and repeated bookkeeping.** Stanli avoids compiling each model
to C++, binds data-dependent structure once, and reuses its reverse plan and
working buffers. [How the graph works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)
explains what is prepared once and what each evaluation repeats.

**Independent observations offer more work to batch.** Regressions, GLMs and
IRT models can turn repeated scalar work into vector operations, amortizing
instruction dispatch. See [loop recovery](how-it-works.md#recovering-vector-operations-from-scalar-loops)
and the [optimization details](../runtime/src/OPTIMIZATIONS.md).

**Shared kernels and sequential work leave less overhead to remove.** Dense
linear algebra and ODEs spend substantial time in Stan Math in both engines.
Recurrences limit independent batching, and fallback derivatives can retain
local autodiff costs. These costs can bring results close to parity or make
Stanli slower; the per-model measurements show where that happens.

<a id="full-corpus"></a>

## Full corpus results

Every application fixture uses the same run settings. Language-conformance
fixtures belong to the [numerical corpus](corpus-status.md) and are not timed
here. This table uses default CmdStan compiler settings; the
[appendix comparison](#cmdstan-with-stanc3-loop-vectorization) enables O1
optimizations and loop vectorization. Incomplete results follow each table.

<!--gen:benchmark_catalog-->
Run `f81381a8b3b6b2ba` (2026-09-21): 319 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `2pl_latent_reg_irt` | 2.09x | 0.1471 | 4.505 |
| `GLMM1_model` | 3.17x | 0.03318 | 2.065 |
| `GLMM_Poisson_model` | 2.37x | 0.009841 | 2.597 |
| `GLM_Binomial_model` | 1.56x | 0.008804 | 2.04 |
| `GLM_Poisson_model` | 2.90x | 0.008244 | 2.026 |
| `M0_model` | 14.08x | 0.01198 | 1.465 |
| `Mb_model` | 1.26x | 0.1252 | 2.131 |
| `Mh_model` | 2.45x | 0.04725 | 2.134 |
| `Mt_model` | 17.51x | 0.01177 | 2.237 |
| `Mtbh_model` | 3.51x | 0.05299 | 3.869 |
| `Mth_model` | 4.33x | 0.0663 | 3.275 |
| `Rate_1_model` | 1.38x | 0.006177 | 1.123 |
| `Rate_2_model` | 1.36x | 0.007088 | 1.27 |
| `Rate_3_model` | 1.31x | 0.005993 | 1.144 |
| `Rate_4_model` | 1.51x | 0.006407 | 1.243 |
| `Rate_5_model` | 1.34x | 0.006692 | 1.238 |
| `Survey_model` | 1.15x | 0.1274 | 1.84 |
| `aalto_bern` | 1.43x | 0.006136 | 1.28 |
| `aalto_binom` | 1.36x | 0.005789 | 1.124 |
| `aalto_binom2` | 1.32x | 0.006407 | 1.243 |
| `aalto_binomb` | 1.38x | 0.005964 | 1.155 |
| `aalto_gpareto` | 0.72x | 0.0133 | 1.618 |
| `aalto_grp_aov` | 1.62x | 0.007525 | 1.827 |
| `aalto_grp_prior_mean` | 1.59x | 0.007813 | 1.891 |
| `aalto_grp_prior_mean_var` | 1.47x | 0.008425 | 2.67 |
| `aalto_lin` | 1.72x | 0.007738 | 1.836 |
| `aalto_lin_std` | 1.71x | 0.008571 | 2.055 |
| `aalto_lin_std_t` | 1.60x | 0.009201 | 2.266 |
| `aalto_poisson_hurdle` | 25.57x | 0.02795 | 1.772 |
| `aalto_poisson_simple` | 1.04x | 0.009999 | 1.383 |
| `accel_gp` | 1.96x | 0.031 | 4.251 |
| `accel_splines` | 1.92x | 0.02572 | 3.219 |
| `arK` | 6.65x | 0.01236 | 1.626 |
| `arma11` | 1.17x | 0.01812 | 1.717 |
| `blr` | 1.78x | 0.008483 | 1.901 |
| `bones_model` | 1.32x | 0.1049 | 2.356 |
| `bym2_offset_only` | 2.88x | 0.08448 | 3.179 |
| `ch09_m5_8s` | 3.74x | 0.008185 | 1.588 |
| `ch09_m5_8s2` | 3.58x | 0.008644 | 1.592 |
| `ch09_m9_1` | 2.49x | 0.009477 | 1.806 |
| `ch09_m9_1_chains4` | 2.43x | 0.009619 | 1.812 |
| `ch09_m9_2` | 1.47x | 0.006638 | 1.259 |
| `ch09_m9_3` | 1.43x | 0.006247 | 1.254 |
| `ch09_m9_4` | 1.27x | 0.006715 | 1.293 |
| `ch09_m9_5` | 1.32x | 0.007277 | 1.29 |
| `ch09_mp` | 2.02x | 0.005617 | 1.055 |
| `ch11_m11_10` | 1.49x | 0.008349 | 1.983 |
| `ch11_m11_11` | 1.65x | 0.008926 | 2.315 |
| `ch11_m11_4` | 1.76x | 0.02593 | 1.775 |
| `ch11_m11_5` | 2.18x | 0.02849 | 1.884 |
| `ch11_m11_6` | 1.34x | 0.009231 | 1.767 |
| `ch11_m11_7` | 1.42x | 0.00714 | 1.594 |
| `ch11_m11_8` | 1.31x | 0.007665 | 1.67 |
| `ch11_m11_9` | 1.51x | 0.006979 | 1.324 |
| `ch11_m_pois` | 1.38x | 0.00629 | 1.258 |
| `ch12_m12_1` | 1.31x | 0.008993 | 1.966 |
| `ch12_m12_2` | 1.31x | 0.01052 | 2.744 |
| `ch12_m12_3` | 16.99x | 0.01119 | 1.311 |
| `ch12_m12_3_alt` | 5.70x | 0.01486 | 1.286 |
| `ch12_m12_4` | 112.51x | 0.06808 | 7.691 |
| `ch12_m12_5` | 1.19x | 5.512 | 8.763 |
| `ch12_m12_6` | 1.16x | 5.903 | 9.78 |
| `ch12_m12_7` | 1.20x | 5.512 | 8.821 |
| `ch13_m13_1` | 1.33x | 0.009431 | 1.718 |
| `ch13_m13_2` | 1.27x | 0.009978 | 1.886 |
| `ch13_m13_3` | 1.32x | 0.009865 | 1.77 |
| `ch13_m13_4` | 1.68x | 0.03335 | 2.203 |
| `ch13_m13_4b` | 1.74x | 0.02849 | 2.075 |
| `ch13_m13_4nc` | 2.68x | 0.02972 | 2.112 |
| `ch13_m13_5` | 1.72x | 0.027 | 2.024 |
| `ch13_m13_6` | 1.72x | 0.03337 | 2.176 |
| `ch13_m13_7` | 1.70x | 0.006227 | 1.054 |
| `ch13_m13_7nc` | 1.98x | 0.006223 | 1.083 |
| `ch14_m14_1` | 1.16x | 0.02226 | 5.777 |
| `ch14_m14_10` | 1.25x | 0.7415 | 3.787 |
| `ch14_m14_11` | 1.20x | 1.249 | 4.176 |
| `ch14_m14_2` | 1.25x | 0.05153 | 5.975 |
| `ch14_m14_3` | 1.78x | 0.04084 | 4.786 |
| `ch14_m14_4` | 3.41x | 0.01064 | 1.549 |
| `ch14_m14_4x` | 3.29x | 0.01069 | 1.538 |
| `ch14_m14_5` | 4.39x | 0.0132 | 1.603 |
| `ch14_m14_6` | 1.23x | 0.2226 | 5.744 |
| `ch14_m14_6x` | 1.28x | 0.2193 | 5.746 |
| `ch14_m14_7` | 1.46x | 0.06338 | 7.802 |
| `ch14_m14_8` | 1.31x | 0.01576 | 3.3 |
| `ch14_m14_8nc` | 1.40x | 0.01654 | 4.516 |
| `ch14_m14_9` | 1.24x | 0.7041 | 3.701 |
| `ch15_m15_1` | 2.68x | 0.009229 | 1.948 |
| `ch15_m15_2` | 2.27x | 0.01084 | 2.172 |
| `ch15_m15_3` | 1.27x | 0.05795 | 1.455 |
| `ch15_m15_4` | 1.27x | 0.04763 | 1.431 |
| `ch15_m15_5` | 2.34x | 0.009704 | 1.975 |
| `ch15_m15_6` | 1.94x | 0.00846 | 1.719 |
| `ch15_m15_7` | 1.37x | 0.03038 | 6.035 |
| `ch15_m15_8` | 2.25x | 0.01445 | 1.597 |
| `ch15_m15_9` | 2.18x | 0.01596 | 1.763 |
| `ch16_m16_1` | 2.31x | 0.03612 | 1.8 |
| `ch16_m16_4` | 3.21x | 0.01066 | 1.65 |
| `covid19imperial_v2` | 1.51x | 1.424 | 6.38 |
| `covid19imperial_v3` | 1.50x | 1.404 | 6.353 |
| `diamonds` | 0.99x | 0.09327 | 2.275 |
| `dogs` | 11.54x | 0.03941 | 2.522 |
| `dogs_hierarchical` | 3.06x | 0.04295 | 1.568 |
| `dogs_nonhierarchical` | 2.62x | 0.05988 | 5.821 |
| `dugongs_model` | 1.83x | 0.008918 | 1.767 |
| `earn_height` | 2.48x | 0.01567 | 1.554 |
| `eight_schools_centered` | 1.42x | 0.007399 | 1.721 |
| <code>eight_schools_<br>noncentered</code> | 1.82x | 0.007673 | 2.03 |
| `election88_full` | 4.12x | 0.4805 | 4.798 |
| `extra_hurdle_poisson` | 2.22x | 0.007234 | 1.317 |
| `garch11` | 1.12x | 0.02369 | 1.699 |
| `gp_pois_regr` | 1.39x | 0.01135 | 4.575 |
| `gp_regr` | 1.44x | 0.01153 | 4.261 |
| `gpcm_latent_reg_irt` | 15.58x | 0.3423 | 7.591 |
| `grsm_latent_reg_irt` | 13.73x | 0.1801 | 5.832 |
| `hier_2pl` | 2.13x | 0.4127 | 6.508 |
| `hierarchical_gp` | 2.22x | 0.06843 | 7.998 |
| `hmm_drive_0` | 1.15x | 0.2957 | 3.861 |
| `hmm_drive_1` | 1.23x | 0.3001 | 3.853 |
| `hmm_example` | 1.61x | 0.05143 | 2.935 |
| `hmm_gaussian` | 1.48x | 0.5487 | 4.155 |
| `i319_gauss_re` | 1.12x | 0.02192 | 3.386 |
| `i319_negbin_fixed` | 1.02x | 0.02501 | 2.173 |
| `i319_negbin_re` | 1.14x | 0.03474 | 3.569 |
| `i319_pois_fixed` | 1.05x | 0.01335 | 1.866 |
| `i319_pois_re` | 1.33x | 0.0229 | 3.285 |
| `i319_pois_re2` | 1.59x | 0.03251 | 3.65 |
| `i320_gp_expquad` | 1.29x | 0.03472 | 5.93 |
| `i320_gp_matern32` | 0.89x | 0.07618 | 5.848 |
| `i320_mi_nhanes` | 1.18x | 0.0175 | 3.212 |
| `i320_pois_trunc_both` | 1.02x | 0.07134 | 2.404 |
| `i320_pois_trunc_ub` | 1.05x | 0.05902 | 2.31 |
| `i320_sratio_cs` | 1.33x | 0.187 | 4.022 |
| `i320_sratio_plain` | 1.26x | 0.1499 | 2.694 |
| `iohmm_reg` | 1.95x | 0.6052 | 5.568 |
| `irt_2pl` | 2.03x | 0.04286 | 2.985 |
| `kidscore_interaction` | 3.57x | 0.01252 | 1.78 |
| `kidscore_interaction_c` | 3.51x | 0.01242 | 1.789 |
| `kidscore_interaction_c2` | 3.49x | 0.01312 | 1.752 |
| `kidscore_interaction_z` | 3.64x | 0.01292 | 1.858 |
| `kidscore_mom_work` | 3.40x | 0.01384 | 1.692 |
| `kidscore_momhs` | 2.37x | 0.01044 | 1.611 |
| `kidscore_momhsiq` | 3.04x | 0.01181 | 1.694 |
| `kidscore_momiq` | 2.27x | 0.009984 | 1.602 |
| `kilpisjarvi` | 2.23x | 0.00797 | 1.55 |
| `ldaK2` | 2.34x | 0.1197 | 2.739 |
| `ldaK5` | 2.63x | 5.306 | 15.18 |
| `log10earn_height` | 2.42x | 0.01579 | 1.545 |
| `logearn_height` | 2.49x | 0.016 | 1.606 |
| `logearn_height_male` | 3.09x | 0.01892 | 1.708 |
| `logearn_interaction` | 3.39x | 0.02361 | 1.808 |
| `logearn_interaction_z` | 3.47x | 0.02254 | 1.915 |
| `logearn_logheight_male` | 3.17x | 0.0191 | 1.701 |
| `logistic_regression_rhs` | 2.36x | 0.1052 | 3.893 |
| `logmesquite` | 4.01x | 0.01021 | 1.886 |
| `logmesquite_logva` | 3.18x | 0.008899 | 1.905 |
| `logmesquite_logvas` | 3.89x | 0.009234 | 2.13 |
| `logmesquite_logvash` | 3.97x | 0.009135 | 2.055 |
| `logmesquite_logvolume` | 2.21x | 0.008808 | 1.739 |
| `losscurve_sislob` | 2.16x | 0.01715 | 3.071 |
| `lotka_volterra` | 2.08x | 0.0566 | 3.179 |
| `low_dim_gauss_mix` | 2.08x | 0.1117 | 2.166 |
| <code>low_dim_gauss_mix_<br>collapse</code> | 2.00x | 0.1125 | 2.067 |
| `lsat_model` | 2.32x | 0.09035 | 2.689 |
| `mesquite` | 3.79x | 0.008574 | 1.827 |
| `multi_occupancy` | 2.48x | 0.07184 | 5.013 |
| `nes` | 4.19x | 0.04642 | 2.1 |
| `nes_logit_model` | 1.02x | 0.01995 | 1.914 |
| `nn_rbm1bJ10` | 1.20x | 0.3413 | 4.665 |
| `nn_rbm1bJ100` | 1.06x | 866.7 | 922.9 |
| `normal_mixture` | 2.08x | 0.1015 | 1.496 |
| `normal_mixture_k` | 1.89x | 0.4504 | 2.974 |
| `one_comp_mm_elim_abs` | 1.06x | 1.013 | 3.245 |
| `pilots` | 1.53x | 0.01035 | 2.152 |
| `prophet` | 2.01x | 0.09211 | 3.877 |
| `radon_county` | 2.31x | 0.08537 | 2.035 |
| `radon_county_intercept` | 8.45x | 0.1516 | 2.965 |
| <code>radon_hierarchical_<br>intercept_centered</code> | 8.47x | 0.2198 | 3.2 |
| <code>radon_hierarchical_<br>intercept_noncentered</code> | 8.84x | 0.2101 | 3.416 |
| <code>radon_partially_pooled_<br>centered</code> | 7.90x | 0.1115 | 2.326 |
| <code>radon_partially_pooled_<br>noncentered</code> | 8.06x | 0.1145 | 2.569 |
| `radon_pooled` | 7.57x | 0.1038 | 2.148 |
| <code>radon_variable_<br>intercept_centered</code> | 8.43x | 0.1522 | 2.724 |
| <code>radon_variable_<br>intercept_noncentered</code> | 8.70x | 0.1552 | 2.925 |
| <code>radon_variable_<br>intercept_slope_centered</code> | 7.63x | 0.1794 | 2.878 |
| <code>radon_variable_<br>intercept_slope_<br>noncentered</code> | 7.56x | 0.1772 | 3.142 |
| <code>radon_variable_slope_<br>centered</code> | 8.13x | 0.1601 | 2.755 |
| <code>radon_variable_slope_<br>noncentered</code> | 8.36x | 0.1625 | 2.918 |
| `rats_model` | 4.68x | 0.01116 | 1.844 |
| `s2_ar_cov` | 1.02x | 0.03192 | 6.037 |
| `s2_beta_binomial` | 1.24x | 0.01764 | 2.516 |
| `s2_car` | 2.02x | 0.01472 | 3.185 |
| `s2_car_esicar` | 2.05x | 0.01446 | 3.298 |
| `s2_car_icar` | 2.24x | 0.01296 | 3.208 |
| `s2_categorical_re` | 1.48x | 0.02675 | 4.241 |
| `s2_cens_interval` | 1.74x | 0.01387 | 2.599 |
| `s2_com_poisson` | 0.21x | 0.6062 | 2.54 |
| `s2_cosy` | 0.99x | 0.03071 | 5.329 |
| `s2_cox` | 3.27x | 0.01225 | 2.942 |
| `s2_cox_cens` | 3.91x | 0.0155 | 3.902 |
| `s2_cumulative_cauchit` | 4.57x | 0.01483 | 2.466 |
| `s2_cumulative_cloglog` | 2.78x | 0.01369 | 2.459 |
| `s2_cumulative_probit` | 1.19x | 0.01858 | 2.5 |
| `s2_custom_vint` | 1.36x | 0.01888 | 2.333 |
| `s2_custom_vreal` | 5.72x | 0.01066 | 2.263 |
| `s2_dirichlet` | 1.04x | 0.03814 | 3.195 |
| `s2_discrete_weibull` | 2.32x | 0.01256 | 2.26 |
| `s2_dist_sigma_re` | 2.26x | 0.01457 | 3.732 |
| `s2_fcor` | 0.90x | 0.0379 | 3.77 |
| `s2_frechet` | 1.50x | 0.01267 | 2.749 |
| `s2_gev` | 0.70x | 0.02615 | 2.587 |
| `s2_gp_approx` | 3.27x | 0.01508 | 4.022 |
| `s2_gp_by_approx` | 3.17x | 0.02015 | 4.81 |
| `s2_gp_by_gr` | 1.35x | 0.04382 | 7.912 |
| `s2_gr_by` | 1.52x | 0.01483 | 3.732 |
| `s2_gr_student` | 1.52x | 0.01445 | 3.967 |
| `s2_hurdle_cumulative` | 1.25x | 0.02625 | 2.961 |
| `s2_hurdle_negbin` | 2.16x | 0.01916 | 2.261 |
| `s2_index_mi` | 1.41x | 0.01447 | 2.991 |
| `s2_logistic_normal` | 1.16x | 0.05774 | 5.236 |
| `s2_me2` | 1.43x | 0.01954 | 5.58 |
| `s2_me2_nomecor` | 1.81x | 0.01559 | 3.193 |
| `s2_mi_lognormal` | 2.14x | 0.01427 | 3.401 |
| `s2_mi_trunc_lb` | 0.95x | 0.02476 | 3.262 |
| `s2_mixture_theta` | 1.31x | 0.02924 | 3.353 |
| `s2_mm` | 1.23x | 0.01663 | 3.459 |
| `s2_mm_weights` | 1.22x | 0.01566 | 3.468 |
| `s2_mmc` | 0.91x | 0.02396 | 5.85 |
| `s2_mo_simo_prior` | 2.32x | 0.01484 | 2.782 |
| `s2_multinomial` | 1.14x | 0.03044 | 2.797 |
| `s2_mv_shared_re` | 1.14x | 0.0243 | 5.995 |
| `s2_mv_subset` | 1.13x | 0.0132 | 2.248 |
| `s2_nl_noloop` | 3.48x | 0.009956 | 2.721 |
| `s2_nlf` | 3.29x | 0.01186 | 2.76 |
| `s2_rate` | 2.37x | 0.01011 | 2.409 |
| `s2_s_by` | 3.38x | 0.0223 | 4.725 |
| `s2_s_cc` | 3.07x | 0.01013 | 3.081 |
| `s2_sar` | 2.63x | 0.01744 | 2.876 |
| `s2_sar_error` | 2.61x | 0.01751 | 2.917 |
| `s2_shifted_lognormal` | 2.71x | 0.01019 | 2.704 |
| `s2_t2_by` | 3.16x | 0.02226 | 4.65 |
| `s2_threading` | 1.28x | 0.01001 | 2.17 |
| `s2_unstr` | 1.05x | 0.0431 | 6.436 |
| `s2_weights_trunc` | 1.09x | 0.02254 | 2.423 |
| `s2_wiener` | 1.02x | 0.07069 | 2.498 |
| `s2_zi_asymlaplace` | 0.89x | 0.02658 | 2.406 |
| `s2_zi_beta` | 1.60x | 0.02174 | 2.451 |
| `s2_zoi_beta` | 1.58x | 0.02272 | 2.471 |
| `seeds_centered_model` | 2.12x | 0.009544 | 2.625 |
| `seeds_model` | 1.79x | 0.009933 | 2.411 |
| `seeds_stanified_model` | 1.73x | 0.009257 | 2.339 |
| `sesame_one_pred_a` | 2.43x | 0.009366 | 1.545 |
| `soil_incubation` | 2.16x | 0.07544 | 2.333 |
| <code>state_space_stochastic_<br>level_stochastic_<br>seasonal</code> | 3.09x | 0.0241 | 3.677 |
| `surgical_model` | 1.30x | 0.008771 | 2.188 |
| `sw_acat` | 3.55x | 0.1065 | 2.997 |
| `sw_acat_cs` | 3.24x | 0.1395 | 4.191 |
| `sw_ar` | 1.68x | 0.01457 | 3.053 |
| `sw_arma` | 1.62x | 0.01696 | 3.168 |
| `sw_asymlaplace` | 0.73x | 0.02024 | 2.341 |
| `sw_bernoulli` | 1.17x | 0.009169 | 2.033 |
| `sw_beta` | 1.57x | 0.01328 | 2.682 |
| `sw_binomial` | 1.38x | 0.01215 | 2.351 |
| `sw_categorical` | 1.12x | 0.01173 | 3.121 |
| `sw_cens` | 2.02x | 0.01349 | 3.098 |
| `sw_cratio` | 1.26x | 0.1467 | 2.554 |
| `sw_cratio_cs` | 1.39x | 0.1863 | 3.744 |
| `sw_cumulative` | 1.01x | 0.0532 | 2.597 |
| `sw_cumulative_cs` | 1.23x | 0.1636 | 3.69 |
| `sw_dist_sigma` | 3.36x | 0.01203 | 2.631 |
| `sw_exgaussian` | 1.85x | 0.01117 | 2.692 |
| `sw_gamma` | 2.48x | 0.009702 | 2.537 |
| `sw_gaussian` | 1.14x | 0.00985 | 2.09 |
| `sw_gp` | 1.23x | 0.05193 | 5.975 |
| `sw_hurdle_gamma` | 2.29x | 0.01534 | 2.344 |
| `sw_hurdle_lognormal` | 2.62x | 0.01566 | 2.36 |
| `sw_hurdle_pois` | 2.66x | 0.01472 | 2.168 |
| `sw_lognormal` | 2.63x | 0.01005 | 2.498 |
| `sw_ma` | 1.69x | 0.01441 | 3.032 |
| `sw_me` | 1.90x | 0.0136 | 2.908 |
| `sw_mi` | 1.14x | 0.01417 | 2.633 |
| `sw_mixture` | 1.39x | 0.02454 | 3.591 |
| `sw_mono` | 2.22x | 0.01451 | 2.754 |
| `sw_mv_norescor` | 1.15x | 0.01308 | 2.257 |
| `sw_mv_rescor` | 1.11x | 0.02852 | 5.077 |
| `sw_negbinomial` | 1.10x | 0.01112 | 2.172 |
| `sw_nonlinear` | 4.47x | 0.01038 | 2.68 |
| `sw_poisson` | 1.16x | 0.008984 | 1.861 |
| `sw_re_bern` | 1.65x | 0.01337 | 3.466 |
| `sw_re_gauss` | 1.45x | 0.01407 | 3.355 |
| `sw_re_negbin` | 1.26x | 0.01588 | 3.543 |
| `sw_re_pois` | 1.56x | 0.01284 | 3.284 |
| `sw_re_slope` | 1.04x | 0.01945 | 5.678 |
| `sw_se` | 2.71x | 0.00983 | 2.307 |
| `sw_skewnormal` | 1.51x | 0.01404 | 2.765 |
| `sw_spline_s` | 3.36x | 0.01114 | 3.252 |
| `sw_spline_t2` | 3.11x | 0.01477 | 3.696 |
| `sw_sratio` | 1.32x | 0.1448 | 2.551 |
| `sw_student` | 2.53x | 0.01021 | 2.637 |
| `sw_trunc` | 1.14x | 0.02199 | 2.377 |
| `sw_vonmises` | 2.14x | 0.01003 | 2.493 |
| `sw_weibull` | 1.49x | 0.01283 | 2.675 |
| `sw_weights` | 3.26x | 0.01084 | 2.294 |
| `sw_zi_binomial` | 1.78x | 0.01698 | 2.196 |
| `sw_zi_negbin` | 1.61x | 0.01835 | 2.229 |
| `sw_zi_poisson` | 2.21x | 0.01494 | 2.14 |
| `wells_daae_c_model` | 1.04x | 0.05084 | 2.197 |
| `wells_dae_c_model` | 1.02x | 0.04681 | 2.181 |
| `wells_dae_inter_model` | 0.99x | 0.05344 | 2.162 |
| `wells_dae_model` | 0.99x | 0.04911 | 2.023 |
| `wells_dist` | 1.84x | 0.0513 | 1.752 |
| `wells_dist100_model` | 1.01x | 0.04412 | 2.008 |
| `wells_dist100ars_model` | 1.04x | 0.04483 | 2.013 |
| <code>wells_interaction_c_<br>model</code> | 1.01x | 0.05052 | 2.126 |
| `wells_interaction_model` | 1.01x | 0.04862 | 2.071 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2269) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.0063 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 4832) |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 5563) |
<!--/gen-->

## Appendix

The [main table](#full-corpus) uses default CmdStan compiler flags.
The appendix table below uses stanc3 `--O1` with loop vectorization enabled.
Both experiments cover the same application inputs with the same Stanli build.

### Configuration and evidence

| Item | Configuration |
| --- | --- |
| Scope | All 319 application models, `--corpus all` |
| Gradient trials | Six alternating pairs, 200 ms warmup and 250 ms measurement per process |
| Displayed workload | Setup + 2,000 × median warm gradient time |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled, lite mode disabled |
| Upstream source base | `2c9d67b98b196e800dc051460aaaf653ac811ace`; exact measured checkpoint and hashes in manifests |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan | 2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6` |
| Stan Math | `5252d51d47c1d5e78005fc043ad996fad6dd8da8` |
| Timeouts | Gradient command 60 s; build 900 s |
| Host | Apple M3 Ultra, macOS arm64, 32 logical CPUs, 96 GiB RAM |

The sweeps run serially on a shared development host with normal background
services. Commands request one thread for Stan, OpenMP and common BLAS
implementations. The manifests retain machine details, requested threads,
source/data hashes, executable hashes, upstream identities and compiler flags;
event logs retain load averages and exact commands.

[Default evidence](../output/corpus-performance/README.md) and
[optimized-reference evidence](../output/corpus-performance-vectorized/README.md)
contain the complete inventories, immutable manifests, summary TSVs, raw paired
observations, command events and checksums. Every model remains in the tables,
including numerical failures and timeouts in the incomplete-results tables.
Current results are never filled from older experiments.

### What the numbers include

**Warm gradients:** both engines evaluate the sampling log density with
proportional terms and Jacobian adjustment at coordinate `i` equal to
`0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Construction and the initial validation
evaluation are outside the timed window. Each accepted pair must agree on the
log density and every gradient component within scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Non-finite values, unequal widths
and failed commands are rejected. This gate is distinct from the independent
[three-point numerical replay](../TESTING.md#comparison-with-cmdstan-on-complete-models).

The gradient ratio is the median of six within-pair CmdStan/Stanli ratios.
The artifacts retain median absolute deviations (MAD) for these ratios and
for each engine's gradient latency. The paired ratio can differ from a ratio
of the median latencies or total-time estimates. Small differences should be
read with the dispersion recorded in the artifacts.

**Setup:** Stanli includes a source-to-MIR compiler-probe process and the
median of six preparation measurements from existing MIR/JSON to a bound
executor. CmdStan includes stanc translation and a fresh ordinary C++ model
build. CmdStan data/model initialization is not added to the estimate.
Common dependencies and precompiled headers are prepared in advance.
Gradient-driver compilation is retained in the event log but excluded from
the estimate. Compilation phases are measured once, with normal filesystem
caches; this is not an in-process Python/R or cold-cache first-fit measurement.

**Total sampling estimate:** each engine's setup time plus 2,000 times its
median warm gradient time. No sampler is run, and adaptation, tree building,
generated quantities and CSV output are excluded. The retained v4 summaries
and manifests use a 20,000-gradient budget; the tables on this page recalculate
totals for 2,000 gradients from the same validated setup and latency measurements.
A missing setup component leaves that estimate blank without hiding a valid
gradient measurement in the incomplete-results table. Sampler correctness tests
remain part of [the validation suite](../TESTING.md).

The fixed-point gate leaves four default-run rows without estimates:
`dogs_log`, `s2_invgaussian` and `sir` report non-finite density or gradients;
`kronecker_gp` has scaled error `0.0063`. The optimized-reference run also
rejects `s2_gp_by_gr` at `5.33e-8`. The incomplete-results tables retain these
recorded failures; see the [numerical policies and exceptions](../TESTING.md#comparison-with-cmdstan-on-complete-models).

### Reproduction

Prepare the compiler/dependencies and build `bench_grad` as described in the
[protocol](benchmark-protocol.md). Use distinct output paths for the two runs:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/default-v4.tsv --bench build/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/vectorized-v4.tsv --bench build/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900 \
  --stanc build/corpus-current/stanc-o1vec-src/_build/default/src/stanc/stanc.exe \
  --stancflags=--O1
```

Run the commands serially. `--resume` requires identical sources, inputs,
binaries and settings. Publish the complete runs with
`tools/publish_corpus_bench.py`, then regenerate the docs and browser catalog:

```sh
python3 tools/publish_corpus_bench.py build/corpus-current/default-v4.tsv \
  output/corpus-performance
python3 tools/publish_corpus_bench.py build/corpus-current/vectorized-v4.tsv \
  output/corpus-performance-vectorized --compiler-evidence build/corpus-current
python3 tools/gen_docs.py
python3 tools/gen_web_models.py deps/posteriordb
```

### CmdStan with stanc3 loop vectorization

Stock pinned stanc3 leaves loop vectorization off at `--O1`. This experiment
changes that one setting and invokes the resulting compiler with `--O1`.
Compared with the default table, it enables **O1 optimizations plus loop
vectorization**; it is not a vectorization-only ablation or `--Oexperimental`.
Both engines are remeasured with the same inputs, Stanli binaries, timing
windows and numerical gate. The table uses the same columns and
2,000-gradient estimate as the default comparison.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
and stock/patched optimized MIR for a scalar normal-likelihood loop are retained
with the [evidence](../output/corpus-performance-vectorized/README.md).

<!--gen:benchmark_vectorized_catalog-->
Run `7bc507e14400797e`: 319 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `2pl_latent_reg_irt` | 1.59x | 0.1506 | 4.448 |
| `GLMM1_model` | 3.23x | 0.03187 | 2.044 |
| `GLMM_Poisson_model` | 1.88x | 0.008971 | 2.591 |
| `GLM_Binomial_model` | 1.33x | 0.0088 | 2.009 |
| `GLM_Poisson_model` | 2.05x | 0.00835 | 2.034 |
| `M0_model` | 14.78x | 0.01227 | 1.44 |
| `Mb_model` | 1.24x | 0.126 | 2.112 |
| `Mh_model` | 2.27x | 0.04644 | 2.103 |
| `Mt_model` | 16.49x | 0.01273 | 2.269 |
| `Mtbh_model` | 3.17x | 0.05324 | 3.77 |
| `Mth_model` | 4.67x | 0.0684 | 3.279 |
| `Rate_1_model` | 1.34x | 0.005881 | 1.125 |
| `Rate_2_model` | 1.35x | 0.006651 | 1.262 |
| `Rate_3_model` | 1.20x | 0.006139 | 1.147 |
| `Rate_4_model` | 1.45x | 0.006627 | 1.243 |
| `Rate_5_model` | 1.32x | 0.006163 | 1.243 |
| `Survey_model` | 1.13x | 0.128 | 1.843 |
| `aalto_bern` | 1.44x | 0.006689 | 1.279 |
| `aalto_binom` | 1.39x | 0.006081 | 1.118 |
| `aalto_binom2` | 1.33x | 0.006826 | 1.246 |
| `aalto_binomb` | 1.36x | 0.006406 | 1.148 |
| `aalto_gpareto` | 0.72x | 0.01419 | 1.812 |
| `aalto_grp_aov` | 1.36x | 0.007369 | 1.781 |
| `aalto_grp_prior_mean` | 1.31x | 0.007459 | 1.869 |
| `aalto_grp_prior_mean_var` | 1.33x | 0.008509 | 2.603 |
| `aalto_lin` | 1.62x | 0.008199 | 1.755 |
| `aalto_lin_std` | 1.56x | 0.00855 | 1.968 |
| `aalto_lin_std_t` | 1.49x | 0.009383 | 2.21 |
| `aalto_poisson_hurdle` | 25.73x | 0.02719 | 1.791 |
| `aalto_poisson_simple` | 1.03x | 0.009281 | 1.384 |
| `accel_gp` | 1.76x | 0.0314 | 5.342 |
| `accel_splines` | 1.57x | 0.0239 | 3.364 |
| `arK` | 4.84x | 0.01243 | 1.621 |
| `arma11` | 0.83x | 0.0175 | 1.708 |
| `blr` | 0.83x | 0.008523 | 1.808 |
| `bones_model` | 1.31x | 0.1054 | 2.366 |
| `bym2_offset_only` | 1.69x | 0.08582 | 3.259 |
| `ch09_m5_8s` | 2.00x | 0.00865 | 1.78 |
| `ch09_m5_8s2` | 1.95x | 0.00869 | 1.764 |
| `ch09_m9_1` | 2.05x | 0.009674 | 2.035 |
| `ch09_m9_1_chains4` | 2.10x | 0.009726 | 2.052 |
| `ch09_m9_2` | 1.46x | 0.006385 | 1.254 |
| `ch09_m9_3` | 1.43x | 0.006204 | 1.253 |
| `ch09_m9_4` | 1.33x | 0.00712 | 1.289 |
| `ch09_m9_5` | 1.28x | 0.006998 | 1.29 |
| `ch09_mp` | 2.00x | 0.006093 | 1.028 |
| `ch11_m11_10` | 1.32x | 0.008464 | 2.059 |
| `ch11_m11_11` | 1.64x | 0.00858 | 2.32 |
| `ch11_m11_4` | 1.72x | 0.02617 | 1.78 |
| `ch11_m11_5` | 2.19x | 0.0273 | 1.863 |
| `ch11_m11_6` | 1.26x | 0.008968 | 1.769 |
| `ch11_m11_7` | 1.39x | 0.006977 | 1.587 |
| `ch11_m11_8` | 1.32x | 0.007572 | 1.667 |
| `ch11_m11_9` | 1.54x | 0.006875 | 1.33 |
| `ch11_m_pois` | 1.44x | 0.006684 | 1.246 |
| `ch12_m12_1` | 1.25x | 0.008967 | 1.944 |
| `ch12_m12_2` | 1.27x | 0.009672 | 2.756 |
| `ch12_m12_3` | 17.19x | 0.01089 | 1.275 |
| `ch12_m12_3_alt` | 5.60x | 0.01497 | 1.283 |
| `ch12_m12_4` | 104.92x | 0.07045 | 7.436 |
| `ch12_m12_5` | 1.13x | 5.545 | 8.527 |
| `ch12_m12_6` | 1.02x | 6.185 | 9.275 |
| `ch12_m12_7` | 1.12x | 5.338 | 8.215 |
| `ch13_m13_1` | 1.25x | 0.009437 | 1.7 |
| `ch13_m13_2` | 1.30x | 0.01012 | 1.875 |
| `ch13_m13_3` | 1.30x | 0.00966 | 1.77 |
| `ch13_m13_4` | 1.69x | 0.03255 | 2.189 |
| `ch13_m13_4b` | 1.72x | 0.0285 | 2.079 |
| `ch13_m13_4nc` | 2.10x | 0.02867 | 2.083 |
| `ch13_m13_5` | 1.77x | 0.02687 | 2.008 |
| `ch13_m13_6` | 1.69x | 0.03261 | 2.179 |
| `ch13_m13_7` | 1.89x | 0.005796 | 1.06 |
| `ch13_m13_7nc` | 1.99x | 0.00627 | 1.096 |
| `ch14_m14_1` | 1.13x | 0.02221 | 5.723 |
| `ch14_m14_10` | 1.28x | 0.7435 | 3.951 |
| `ch14_m14_11` | 1.15x | 1.224 | 4.576 |
| `ch14_m14_2` | 1.20x | 0.05223 | 5.974 |
| `ch14_m14_3` | 1.83x | 0.04129 | 4.754 |
| `ch14_m14_4` | 1.76x | 0.0108 | 1.621 |
| `ch14_m14_4x` | 1.82x | 0.01081 | 1.61 |
| `ch14_m14_5` | 2.07x | 0.01272 | 1.768 |
| `ch14_m14_6` | 1.21x | 0.2214 | 5.834 |
| `ch14_m14_6x` | 1.26x | 0.216 | 5.855 |
| `ch14_m14_7` | 1.39x | 0.06439 | 7.732 |
| `ch14_m14_8` | 1.30x | 0.01466 | 3.554 |
| `ch14_m14_8nc` | 1.37x | 0.01647 | 4.788 |
| `ch14_m14_9` | 1.25x | 0.7001 | 3.891 |
| `ch15_m15_1` | 1.87x | 0.008805 | 2.161 |
| `ch15_m15_2` | 1.54x | 0.01006 | 2.413 |
| `ch15_m15_3` | 0.97x | 0.05641 | 1.407 |
| `ch15_m15_4` | 0.94x | 0.04876 | 1.415 |
| `ch15_m15_5` | 2.17x | 0.009586 | 2.444 |
| `ch15_m15_6` | 1.62x | 0.007959 | 1.895 |
| `ch15_m15_7` | 1.36x | 0.03155 | 6.443 |
| `ch15_m15_8` | 1.89x | 0.01435 | 1.598 |
| `ch15_m15_9` | 1.84x | 0.01658 | 1.884 |
| `ch16_m16_1` | 2.34x | 0.03486 | 1.812 |
| `ch16_m16_4` | 1.94x | 0.01078 | 2.107 |
| `covid19imperial_v2` | 1.48x | 1.416 | 6.278 |
| `covid19imperial_v3` | 1.43x | 1.432 | 6.283 |
| `diamonds` | 0.95x | 0.09242 | 2.258 |
| `dogs` | 4.68x | 0.03882 | 2.993 |
| `dogs_hierarchical` | 3.22x | 0.04363 | 1.578 |
| `dogs_nonhierarchical` | 2.62x | 0.05707 | 5.754 |
| `dugongs_model` | 1.83x | 0.008424 | 1.76 |
| `earn_height` | 1.73x | 0.016 | 1.598 |
| `eight_schools_centered` | 1.39x | 0.007624 | 1.718 |
| <code>eight_schools_<br>noncentered</code> | 1.38x | 0.007198 | 1.967 |
| `election88_full` | 2.09x | 0.4612 | 4.784 |
| `extra_hurdle_poisson` | 2.16x | 0.007146 | 1.331 |
| `garch11` | 0.94x | 0.02339 | 1.691 |
| `gp_pois_regr` | 1.33x | 0.01174 | 4.439 |
| `gp_regr` | 1.33x | 0.01156 | 4.246 |
| `gpcm_latent_reg_irt` | 15.58x | 0.3363 | 7.924 |
| `grsm_latent_reg_irt` | 13.73x | 0.1766 | 6.054 |
| `hier_2pl` | 1.58x | 0.4223 | 6.272 |
| `hierarchical_gp` | 2.36x | 0.06756 | 7.974 |
| `hmm_drive_0` | 1.14x | 0.2924 | 3.855 |
| `hmm_drive_1` | 1.23x | 0.3005 | 3.85 |
| `hmm_example` | 1.60x | 0.05312 | 2.912 |
| `hmm_gaussian` | 1.39x | 0.5738 | 4.175 |
| `i319_gauss_re` | 1.19x | 0.02134 | 3.399 |
| `i319_negbin_fixed` | 0.96x | 0.02585 | 2.202 |
| `i319_negbin_re` | 1.18x | 0.03491 | 3.596 |
| `i319_pois_fixed` | 1.00x | 0.01327 | 1.9 |
| `i319_pois_re` | 1.33x | 0.02356 | 3.329 |
| `i319_pois_re2` | 1.44x | 0.03151 | 3.653 |
| `i320_gp_expquad` | 1.25x | 0.03441 | 6.148 |
| `i320_gp_matern32` | 0.87x | 0.07653 | 6.097 |
| `i320_mi_nhanes` | 1.22x | 0.01872 | 3.226 |
| `i320_pois_trunc_both` | 0.99x | 0.07407 | 2.375 |
| `i320_pois_trunc_ub` | 1.06x | 0.0582 | 2.248 |
| `i320_sratio_cs` | 1.81x | 0.1904 | 4.437 |
| `i320_sratio_plain` | 1.20x | 0.1447 | 2.962 |
| `iohmm_reg` | 1.85x | 0.5993 | 5.714 |
| `irt_2pl` | 1.26x | 0.04451 | 3.018 |
| `kidscore_interaction` | 2.24x | 0.01266 | 1.936 |
| `kidscore_interaction_c` | 2.29x | 0.01262 | 1.953 |
| `kidscore_interaction_c2` | 2.26x | 0.01249 | 1.915 |
| `kidscore_interaction_z` | 2.26x | 0.01259 | 1.994 |
| `kidscore_mom_work` | 2.20x | 0.01325 | 1.843 |
| `kidscore_momhs` | 1.71x | 0.0101 | 1.64 |
| `kidscore_momhsiq` | 2.15x | 0.01163 | 1.814 |
| `kidscore_momiq` | 1.68x | 0.01003 | 1.658 |
| `kilpisjarvi` | 1.63x | 0.008012 | 1.535 |
| `ldaK2` | 2.36x | 0.1184 | 2.657 |
| `ldaK5` | 2.51x | 5.393 | 14.69 |
| `log10earn_height` | 1.76x | 0.01648 | 1.638 |
| `logearn_height` | 1.66x | 0.01611 | 1.641 |
| `logearn_height_male` | 2.09x | 0.01971 | 1.818 |
| `logearn_interaction` | 2.18x | 0.02338 | 1.948 |
| `logearn_interaction_z` | 2.19x | 0.0231 | 2.067 |
| `logearn_logheight_male` | 2.12x | 0.01905 | 1.824 |
| `logistic_regression_rhs` | 1.19x | 0.104 | 3.929 |
| `logmesquite` | 2.89x | 0.009824 | 2.146 |
| `logmesquite_logva` | 2.29x | 0.009109 | 2.064 |
| `logmesquite_logvas` | 2.79x | 0.009898 | 2.385 |
| `logmesquite_logvash` | 2.68x | 0.009567 | 2.3 |
| `logmesquite_logvolume` | 1.76x | 0.008171 | 1.773 |
| `losscurve_sislob` | 1.92x | 0.01606 | 3.493 |
| `lotka_volterra` | 2.02x | 0.05693 | 3.32 |
| `low_dim_gauss_mix` | 2.02x | 0.1136 | 2.157 |
| <code>low_dim_gauss_mix_<br>collapse</code> | 2.09x | 0.1083 | 2.058 |
| `lsat_model` | 1.62x | 0.08929 | 2.634 |
| `mesquite` | 2.89x | 0.009332 | 2.086 |
| `multi_occupancy` | 2.27x | 0.07486 | 5.418 |
| `nes` | 2.66x | 0.04634 | 2.438 |
| `nes_logit_model` | 1.03x | 0.01983 | 1.937 |
| `nn_rbm1bJ10` | 1.20x | 0.3435 | 4.619 |
| `nn_rbm1bJ100` | 1.07x | 860.2 | 926.9 |
| `normal_mixture` | 2.08x | 0.1011 | 1.745 |
| `normal_mixture_k` | 1.93x | 0.4524 | 2.987 |
| `one_comp_mm_elim_abs` | 1.03x | 1.045 | 3.619 |
| `pilots` | 1.39x | 0.01025 | 2.24 |
| `prophet` | 1.49x | 0.09402 | 4.462 |
| `radon_county` | 2.10x | 0.08635 | 2.031 |
| `radon_county_intercept` | 6.80x | 0.1562 | 2.438 |
| <code>radon_hierarchical_<br>intercept_centered</code> | 6.83x | 0.215 | 2.966 |
| <code>radon_hierarchical_<br>intercept_noncentered</code> | 6.98x | 0.2161 | 3.138 |
| <code>radon_partially_pooled_<br>centered</code> | 7.93x | 0.1145 | 2.35 |
| <code>radon_partially_pooled_<br>noncentered</code> | 7.66x | 0.1164 | 2.537 |
| `radon_pooled` | 1.82x | 0.104 | 1.762 |
| <code>radon_variable_<br>intercept_centered</code> | 6.96x | 0.1557 | 2.535 |
| <code>radon_variable_<br>intercept_noncentered</code> | 6.99x | 0.1575 | 2.774 |
| <code>radon_variable_<br>intercept_slope_centered</code> | 6.20x | 0.1779 | 2.739 |
| <code>radon_variable_<br>intercept_slope_<br>noncentered</code> | 6.21x | 0.1783 | 3.009 |
| <code>radon_variable_slope_<br>centered</code> | 6.86x | 0.1558 | 2.57 |
| <code>radon_variable_slope_<br>noncentered</code> | 6.63x | 0.1639 | 2.77 |
| `rats_model` | 4.07x | 0.01115 | 1.825 |
| `s2_ar_cov` | 1.02x | 0.03229 | 6.415 |
| `s2_beta_binomial` | 1.12x | 0.01745 | 2.47 |
| `s2_car` | 2.00x | 0.01449 | 3.503 |
| `s2_car_esicar` | 1.98x | 0.01454 | 3.573 |
| `s2_car_icar` | 2.37x | 0.01307 | 3.169 |
| `s2_categorical_re` | 1.47x | 0.02895 | 4.224 |
| `s2_cens_interval` | 1.78x | 0.01403 | 2.518 |
| `s2_com_poisson` | 0.17x | 0.6085 | 3.533 |
| `s2_cosy` | 0.97x | 0.03122 | 5.701 |
| `s2_cox` | 3.24x | 0.01321 | 2.855 |
| `s2_cox_cens` | 3.98x | 0.0156 | 3.791 |
| `s2_cumulative_cauchit` | 3.90x | 0.01433 | 2.551 |
| `s2_cumulative_cloglog` | 2.31x | 0.01323 | 2.558 |
| `s2_cumulative_probit` | 1.35x | 0.01883 | 2.46 |
| `s2_custom_vint` | 1.37x | 0.01886 | 2.311 |
| `s2_custom_vreal` | 5.37x | 0.01115 | 2.223 |
| `s2_dirichlet` | 1.05x | 0.03828 | 3.156 |
| `s2_discrete_weibull` | 2.06x | 0.01335 | 2.284 |
| `s2_dist_sigma_re` | 1.87x | 0.0144 | 3.775 |
| `s2_fcor` | 0.95x | 0.03769 | 3.718 |
| `s2_frechet` | 1.15x | 0.01342 | 2.728 |
| `s2_gev` | 0.60x | 0.02658 | 2.645 |
| `s2_gp_approx` | 2.33x | 0.01398 | 4.304 |
| `s2_gp_by_approx` | 3.35x | 0.02155 | 5.333 |
| `s2_gr_by` | 1.49x | 0.01461 | 3.76 |
| `s2_gr_student` | 1.48x | 0.01564 | 4.147 |
| `s2_hurdle_cumulative` | 1.25x | 0.02647 | 2.939 |
| `s2_hurdle_negbin` | 2.12x | 0.01963 | 2.287 |
| `s2_index_mi` | 1.27x | 0.01447 | 2.984 |
| `s2_logistic_normal` | 1.06x | 0.05598 | 5.609 |
| `s2_me2` | 1.39x | 0.01925 | 5.537 |
| `s2_me2_nomecor` | 1.63x | 0.01662 | 3.332 |
| `s2_mi_lognormal` | 1.68x | 0.01374 | 3.37 |
| `s2_mi_trunc_lb` | 0.92x | 0.02521 | 3.217 |
| `s2_mixture_theta` | 1.23x | 0.02961 | 3.23 |
| `s2_mm` | 1.20x | 0.01592 | 3.506 |
| `s2_mm_weights` | 1.22x | 0.01578 | 3.5 |
| `s2_mmc` | 0.88x | 0.02436 | 5.895 |
| `s2_mo_simo_prior` | 1.85x | 0.01485 | 2.909 |
| `s2_multinomial` | 1.13x | 0.03201 | 2.764 |
| `s2_mv_shared_re` | 1.12x | 0.02336 | 5.981 |
| `s2_mv_subset` | 1.08x | 0.01312 | 2.268 |
| `s2_nl_noloop` | 1.62x | 0.009784 | 2.61 |
| `s2_nlf` | 2.23x | 0.01205 | 3.141 |
| `s2_rate` | 1.46x | 0.01015 | 2.356 |
| `s2_s_by` | 2.09x | 0.02307 | 4.499 |
| `s2_s_cc` | 2.03x | 0.01073 | 3.001 |
| `s2_sar` | 2.25x | 0.01754 | 2.995 |
| `s2_sar_error` | 2.22x | 0.01887 | 2.987 |
| `s2_shifted_lognormal` | 2.02x | 0.01031 | 2.698 |
| `s2_t2_by` | 1.77x | 0.02272 | 4.46 |
| `s2_threading` | 1.19x | 0.009955 | 2.174 |
| `s2_unstr` | 1.10x | 0.04288 | 7.621 |
| `s2_weights_trunc` | 1.15x | 0.02174 | 2.373 |
| `s2_wiener` | 1.03x | 0.06984 | 2.494 |
| `s2_zi_asymlaplace` | 0.89x | 0.02545 | 2.468 |
| `s2_zi_beta` | 1.43x | 0.0206 | 2.509 |
| `s2_zoi_beta` | 1.41x | 0.02288 | 2.566 |
| `seeds_centered_model` | 1.87x | 0.009924 | 2.657 |
| `seeds_model` | 1.56x | 0.009551 | 2.479 |
| `seeds_stanified_model` | 1.44x | 0.009981 | 2.383 |
| `sesame_one_pred_a` | 1.80x | 0.009308 | 1.583 |
| `soil_incubation` | 2.13x | 0.07127 | 2.81 |
| <code>state_space_stochastic_<br>level_stochastic_<br>seasonal</code> | 3.00x | 0.02372 | 3.677 |
| `surgical_model` | 1.30x | 0.008074 | 2.181 |
| `sw_acat` | 3.06x | 0.1131 | 3.014 |
| `sw_acat_cs` | 3.45x | 0.1421 | 4.269 |
| `sw_ar` | 1.54x | 0.0146 | 3.042 |
| `sw_arma` | 1.39x | 0.01804 | 3.143 |
| `sw_asymlaplace` | 0.76x | 0.02085 | 2.407 |
| `sw_bernoulli` | 1.16x | 0.008709 | 2.027 |
| `sw_beta` | 1.19x | 0.01321 | 2.695 |
| `sw_binomial` | 1.13x | 0.01224 | 2.261 |
| `sw_categorical` | 1.12x | 0.01292 | 3.156 |
| `sw_cens` | 1.58x | 0.0135 | 3.02 |
| `sw_cratio` | 1.18x | 0.1419 | 2.827 |
| `sw_cratio_cs` | 1.78x | 0.1905 | 4.121 |
| `sw_cumulative` | 0.98x | 0.05368 | 2.571 |
| `sw_cumulative_cs` | 1.79x | 0.1601 | 3.992 |
| `sw_dist_sigma` | 1.82x | 0.01226 | 2.549 |
| `sw_exgaussian` | 1.34x | 0.01174 | 2.645 |
| `sw_gamma` | 1.33x | 0.01043 | 2.531 |
| `sw_gaussian` | 1.10x | 0.009993 | 2.072 |
| `sw_gp` | 1.19x | 0.0525 | 6.181 |
| `sw_hurdle_gamma` | 2.18x | 0.0162 | 2.316 |
| `sw_hurdle_lognormal` | 2.44x | 0.01436 | 2.341 |
| `sw_hurdle_pois` | 2.48x | 0.01548 | 2.186 |
| `sw_lognormal` | 1.66x | 0.01027 | 2.444 |
| `sw_ma` | 1.53x | 0.01453 | 3.014 |
| `sw_me` | 1.67x | 0.01273 | 3.063 |
| `sw_mi` | 1.09x | 0.01332 | 2.636 |
| `sw_mixture` | 1.41x | 0.02373 | 3.546 |
| `sw_mono` | 1.86x | 0.0146 | 2.866 |
| `sw_mv_norescor` | 1.08x | 0.01326 | 2.275 |
| `sw_mv_rescor` | 1.13x | 0.0277 | 4.986 |
| `sw_negbinomial` | 1.03x | 0.01097 | 2.182 |
| `sw_nonlinear` | 1.74x | 0.01052 | 2.673 |
| `sw_poisson` | 1.10x | 0.009682 | 1.878 |
| `sw_re_bern` | 1.61x | 0.01248 | 3.454 |
| `sw_re_gauss` | 1.41x | 0.01329 | 3.387 |
| `sw_re_negbin` | 1.32x | 0.01529 | 3.565 |
| `sw_re_pois` | 1.58x | 0.01371 | 3.31 |
| `sw_re_slope` | 1.06x | 0.0197 | 5.711 |
| `sw_se` | 1.66x | 0.009204 | 2.203 |
| `sw_skewnormal` | 1.39x | 0.01422 | 2.699 |
| `sw_spline_s` | 2.08x | 0.01203 | 3.142 |
| `sw_spline_t2` | 1.77x | 0.01443 | 3.607 |
| `sw_sratio` | 1.17x | 0.143 | 2.827 |
| `sw_student` | 1.75x | 0.01094 | 2.562 |
| `sw_trunc` | 1.12x | 0.02134 | 2.334 |
| `sw_vonmises` | 2.26x | 0.01095 | 2.427 |
| `sw_weibull` | 1.13x | 0.01256 | 2.67 |
| `sw_weights` | 3.25x | 0.01142 | 2.235 |
| `sw_zi_binomial` | 1.77x | 0.01696 | 2.221 |
| `sw_zi_negbin` | 1.57x | 0.01925 | 2.249 |
| `sw_zi_poisson` | 2.03x | 0.01502 | 2.18 |
| `wells_daae_c_model` | 1.06x | 0.0505 | 2.219 |
| `wells_dae_c_model` | 1.03x | 0.04774 | 2.189 |
| `wells_dae_inter_model` | 1.00x | 0.05255 | 2.175 |
| `wells_dae_model` | 1.05x | 0.04866 | 2.052 |
| `wells_dist` | 1.53x | 0.05165 | 1.793 |
| `wells_dist100_model` | 1.01x | 0.04308 | 1.989 |
| `wells_dist100ars_model` | 1.02x | 0.04548 | 2.026 |
| <code>wells_interaction_c_<br>model</code> | 1.04x | 0.04988 | 2.137 |
| `wells_interaction_model` | 0.98x | 0.05054 | 2.079 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2269) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.0063 |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 4816) |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 5547) |
<!--/gen-->
