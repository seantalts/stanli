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
| `2pl_latent_reg_irt` | 2.09x | 0.147 | 4.51 |
| `GLMM1_model` | 3.17x | 0.0332 | 2.06 |
| `GLMM_Poisson_model` | 2.37x | 0.00984 | 2.6 |
| `GLM_Binomial_model` | 1.56x | 0.0088 | 2.04 |
| `GLM_Poisson_model` | 2.9x | 0.00824 | 2.03 |
| `M0_model` | 14.1x | 0.012 | 1.47 |
| `Mb_model` | 1.26x | 0.125 | 2.13 |
| `Mh_model` | 2.45x | 0.0472 | 2.13 |
| `Mt_model` | 17.5x | 0.0118 | 2.24 |
| `Mtbh_model` | 3.51x | 0.053 | 3.87 |
| `Mth_model` | 4.33x | 0.0663 | 3.28 |
| `Rate_1_model` | 1.38x | 0.00618 | 1.12 |
| `Rate_2_model` | 1.36x | 0.00709 | 1.27 |
| `Rate_3_model` | 1.31x | 0.00599 | 1.14 |
| `Rate_4_model` | 1.51x | 0.00641 | 1.24 |
| `Rate_5_model` | 1.34x | 0.00669 | 1.24 |
| `Survey_model` | 1.15x | 0.127 | 1.84 |
| `aalto_bern` | 1.43x | 0.00614 | 1.28 |
| `aalto_binom` | 1.36x | 0.00579 | 1.12 |
| `aalto_binom2` | 1.32x | 0.00641 | 1.24 |
| `aalto_binomb` | 1.38x | 0.00596 | 1.15 |
| `aalto_gpareto` | 0.717x | 0.0133 | 1.62 |
| `aalto_grp_aov` | 1.62x | 0.00753 | 1.83 |
| `aalto_grp_prior_mean` | 1.59x | 0.00781 | 1.89 |
| `aalto_grp_prior_mean_var` | 1.47x | 0.00842 | 2.67 |
| `aalto_lin` | 1.72x | 0.00774 | 1.84 |
| `aalto_lin_std` | 1.71x | 0.00857 | 2.06 |
| `aalto_lin_std_t` | 1.6x | 0.0092 | 2.27 |
| `aalto_poisson_hurdle` | 25.6x | 0.0279 | 1.77 |
| `aalto_poisson_simple` | 1.04x | 0.01 | 1.38 |
| `accel_gp` | 1.96x | 0.031 | 4.25 |
| `accel_splines` | 1.92x | 0.0257 | 3.22 |
| `arK` | 6.65x | 0.0124 | 1.63 |
| `arma11` | 1.17x | 0.0181 | 1.72 |
| `blr` | 1.78x | 0.00848 | 1.9 |
| `bones_model` | 1.32x | 0.105 | 2.36 |
| `bym2_offset_only` | 2.88x | 0.0845 | 3.18 |
| `ch09_m5_8s` | 3.74x | 0.00819 | 1.59 |
| `ch09_m5_8s2` | 3.58x | 0.00864 | 1.59 |
| `ch09_m9_1` | 2.49x | 0.00948 | 1.81 |
| `ch09_m9_1_chains4` | 2.43x | 0.00962 | 1.81 |
| `ch09_m9_2` | 1.47x | 0.00664 | 1.26 |
| `ch09_m9_3` | 1.43x | 0.00625 | 1.25 |
| `ch09_m9_4` | 1.27x | 0.00671 | 1.29 |
| `ch09_m9_5` | 1.32x | 0.00728 | 1.29 |
| `ch09_mp` | 2.02x | 0.00562 | 1.05 |
| `ch11_m11_10` | 1.49x | 0.00835 | 1.98 |
| `ch11_m11_11` | 1.65x | 0.00893 | 2.31 |
| `ch11_m11_4` | 1.76x | 0.0259 | 1.78 |
| `ch11_m11_5` | 2.18x | 0.0285 | 1.88 |
| `ch11_m11_6` | 1.34x | 0.00923 | 1.77 |
| `ch11_m11_7` | 1.42x | 0.00714 | 1.59 |
| `ch11_m11_8` | 1.31x | 0.00767 | 1.67 |
| `ch11_m11_9` | 1.51x | 0.00698 | 1.32 |
| `ch11_m_pois` | 1.38x | 0.00629 | 1.26 |
| `ch12_m12_1` | 1.31x | 0.00899 | 1.97 |
| `ch12_m12_2` | 1.31x | 0.0105 | 2.74 |
| `ch12_m12_3` | 17x | 0.0112 | 1.31 |
| `ch12_m12_3_alt` | 5.7x | 0.0149 | 1.29 |
| `ch12_m12_4` | 113x | 0.0681 | 7.69 |
| `ch12_m12_5` | 1.19x | 5.51 | 8.76 |
| `ch12_m12_6` | 1.16x | 5.9 | 9.78 |
| `ch12_m12_7` | 1.2x | 5.51 | 8.82 |
| `ch13_m13_1` | 1.33x | 0.00943 | 1.72 |
| `ch13_m13_2` | 1.27x | 0.00998 | 1.89 |
| `ch13_m13_3` | 1.32x | 0.00986 | 1.77 |
| `ch13_m13_4` | 1.68x | 0.0333 | 2.2 |
| `ch13_m13_4b` | 1.74x | 0.0285 | 2.08 |
| `ch13_m13_4nc` | 2.68x | 0.0297 | 2.11 |
| `ch13_m13_5` | 1.72x | 0.027 | 2.02 |
| `ch13_m13_6` | 1.72x | 0.0334 | 2.18 |
| `ch13_m13_7` | 1.7x | 0.00623 | 1.05 |
| `ch13_m13_7nc` | 1.98x | 0.00622 | 1.08 |
| `ch14_m14_1` | 1.16x | 0.0223 | 5.78 |
| `ch14_m14_10` | 1.25x | 0.742 | 3.79 |
| `ch14_m14_11` | 1.2x | 1.25 | 4.18 |
| `ch14_m14_2` | 1.25x | 0.0515 | 5.97 |
| `ch14_m14_3` | 1.78x | 0.0408 | 4.79 |
| `ch14_m14_4` | 3.41x | 0.0106 | 1.55 |
| `ch14_m14_4x` | 3.29x | 0.0107 | 1.54 |
| `ch14_m14_5` | 4.39x | 0.0132 | 1.6 |
| `ch14_m14_6` | 1.23x | 0.223 | 5.74 |
| `ch14_m14_6x` | 1.28x | 0.219 | 5.75 |
| `ch14_m14_7` | 1.46x | 0.0634 | 7.8 |
| `ch14_m14_8` | 1.31x | 0.0158 | 3.3 |
| `ch14_m14_8nc` | 1.4x | 0.0165 | 4.52 |
| `ch14_m14_9` | 1.24x | 0.704 | 3.7 |
| `ch15_m15_1` | 2.68x | 0.00923 | 1.95 |
| `ch15_m15_2` | 2.27x | 0.0108 | 2.17 |
| `ch15_m15_3` | 1.27x | 0.0579 | 1.45 |
| `ch15_m15_4` | 1.27x | 0.0476 | 1.43 |
| `ch15_m15_5` | 2.34x | 0.0097 | 1.97 |
| `ch15_m15_6` | 1.94x | 0.00846 | 1.72 |
| `ch15_m15_7` | 1.37x | 0.0304 | 6.03 |
| `ch15_m15_8` | 2.25x | 0.0145 | 1.6 |
| `ch15_m15_9` | 2.18x | 0.016 | 1.76 |
| `ch16_m16_1` | 2.31x | 0.0361 | 1.8 |
| `ch16_m16_4` | 3.21x | 0.0107 | 1.65 |
| `covid19imperial_v2` | 1.51x | 1.42 | 6.38 |
| `covid19imperial_v3` | 1.5x | 1.4 | 6.35 |
| `diamonds` | 0.993x | 0.0933 | 2.27 |
| `dogs` | 11.5x | 0.0394 | 2.52 |
| `dogs_hierarchical` | 3.06x | 0.0429 | 1.57 |
| `dogs_nonhierarchical` | 2.62x | 0.0599 | 5.82 |
| `dugongs_model` | 1.83x | 0.00892 | 1.77 |
| `earn_height` | 2.48x | 0.0157 | 1.55 |
| `eight_schools_centered` | 1.42x | 0.0074 | 1.72 |
| <code>eight_schools_</code><br><code>noncentered</code> | 1.82x | 0.00767 | 2.03 |
| `election88_full` | 4.12x | 0.481 | 4.8 |
| `extra_hurdle_poisson` | 2.22x | 0.00723 | 1.32 |
| `garch11` | 1.12x | 0.0237 | 1.7 |
| `gp_pois_regr` | 1.39x | 0.0113 | 4.57 |
| `gp_regr` | 1.44x | 0.0115 | 4.26 |
| `gpcm_latent_reg_irt` | 15.6x | 0.342 | 7.59 |
| `grsm_latent_reg_irt` | 13.7x | 0.18 | 5.83 |
| `hier_2pl` | 2.13x | 0.413 | 6.51 |
| `hierarchical_gp` | 2.22x | 0.0684 | 8 |
| `hmm_drive_0` | 1.15x | 0.296 | 3.86 |
| `hmm_drive_1` | 1.23x | 0.3 | 3.85 |
| `hmm_example` | 1.61x | 0.0514 | 2.94 |
| `hmm_gaussian` | 1.48x | 0.549 | 4.15 |
| `i319_gauss_re` | 1.12x | 0.0219 | 3.39 |
| `i319_negbin_fixed` | 1.02x | 0.025 | 2.17 |
| `i319_negbin_re` | 1.14x | 0.0347 | 3.57 |
| `i319_pois_fixed` | 1.05x | 0.0133 | 1.87 |
| `i319_pois_re` | 1.33x | 0.0229 | 3.29 |
| `i319_pois_re2` | 1.59x | 0.0325 | 3.65 |
| `i320_gp_expquad` | 1.29x | 0.0347 | 5.93 |
| `i320_gp_matern32` | 0.89x | 0.0762 | 5.85 |
| `i320_mi_nhanes` | 1.18x | 0.0175 | 3.21 |
| `i320_pois_trunc_both` | 1.02x | 0.0713 | 2.4 |
| `i320_pois_trunc_ub` | 1.05x | 0.059 | 2.31 |
| `i320_sratio_cs` | 1.33x | 0.187 | 4.02 |
| `i320_sratio_plain` | 1.26x | 0.15 | 2.69 |
| `iohmm_reg` | 1.95x | 0.605 | 5.57 |
| `irt_2pl` | 2.03x | 0.0429 | 2.99 |
| `kidscore_interaction` | 3.57x | 0.0125 | 1.78 |
| `kidscore_interaction_c` | 3.51x | 0.0124 | 1.79 |
| `kidscore_interaction_c2` | 3.49x | 0.0131 | 1.75 |
| `kidscore_interaction_z` | 3.64x | 0.0129 | 1.86 |
| `kidscore_mom_work` | 3.4x | 0.0138 | 1.69 |
| `kidscore_momhs` | 2.37x | 0.0104 | 1.61 |
| `kidscore_momhsiq` | 3.04x | 0.0118 | 1.69 |
| `kidscore_momiq` | 2.27x | 0.00998 | 1.6 |
| `kilpisjarvi` | 2.23x | 0.00797 | 1.55 |
| `ldaK2` | 2.34x | 0.12 | 2.74 |
| `ldaK5` | 2.63x | 5.31 | 15.2 |
| `log10earn_height` | 2.42x | 0.0158 | 1.55 |
| `logearn_height` | 2.49x | 0.016 | 1.61 |
| `logearn_height_male` | 3.09x | 0.0189 | 1.71 |
| `logearn_interaction` | 3.39x | 0.0236 | 1.81 |
| `logearn_interaction_z` | 3.47x | 0.0225 | 1.91 |
| `logearn_logheight_male` | 3.17x | 0.0191 | 1.7 |
| `logistic_regression_rhs` | 2.36x | 0.105 | 3.89 |
| `logmesquite` | 4.01x | 0.0102 | 1.89 |
| `logmesquite_logva` | 3.18x | 0.0089 | 1.91 |
| `logmesquite_logvas` | 3.89x | 0.00923 | 2.13 |
| `logmesquite_logvash` | 3.97x | 0.00913 | 2.06 |
| `logmesquite_logvolume` | 2.21x | 0.00881 | 1.74 |
| `losscurve_sislob` | 2.16x | 0.0172 | 3.07 |
| `lotka_volterra` | 2.08x | 0.0566 | 3.18 |
| `low_dim_gauss_mix` | 2.08x | 0.112 | 2.17 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2x | 0.112 | 2.07 |
| `lsat_model` | 2.32x | 0.0904 | 2.69 |
| `mesquite` | 3.79x | 0.00857 | 1.83 |
| `multi_occupancy` | 2.48x | 0.0718 | 5.01 |
| `nes` | 4.19x | 0.0464 | 2.1 |
| `nes_logit_model` | 1.02x | 0.02 | 1.91 |
| `nn_rbm1bJ10` | 1.2x | 0.341 | 4.66 |
| `nn_rbm1bJ100` | 1.06x | 867 | 923 |
| `normal_mixture` | 2.08x | 0.101 | 1.5 |
| `normal_mixture_k` | 1.89x | 0.45 | 2.97 |
| `one_comp_mm_elim_abs` | 1.06x | 1.01 | 3.24 |
| `pilots` | 1.53x | 0.0103 | 2.15 |
| `prophet` | 2.01x | 0.0921 | 3.88 |
| `radon_county` | 2.31x | 0.0854 | 2.04 |
| `radon_county_intercept` | 8.45x | 0.152 | 2.96 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 8.47x | 0.22 | 3.2 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 8.84x | 0.21 | 3.42 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 7.9x | 0.111 | 2.33 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 8.06x | 0.114 | 2.57 |
| `radon_pooled` | 7.57x | 0.104 | 2.15 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.43x | 0.152 | 2.72 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 8.7x | 0.155 | 2.92 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.63x | 0.179 | 2.88 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.56x | 0.177 | 3.14 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.13x | 0.16 | 2.76 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 8.36x | 0.162 | 2.92 |
| `rats_model` | 4.68x | 0.0112 | 1.84 |
| `s2_ar_cov` | 1.02x | 0.0319 | 6.04 |
| `s2_beta_binomial` | 1.24x | 0.0176 | 2.52 |
| `s2_car` | 2.02x | 0.0147 | 3.18 |
| `s2_car_esicar` | 2.05x | 0.0145 | 3.3 |
| `s2_car_icar` | 2.24x | 0.013 | 3.21 |
| `s2_categorical_re` | 1.48x | 0.0268 | 4.24 |
| `s2_cens_interval` | 1.74x | 0.0139 | 2.6 |
| `s2_com_poisson` | 0.209x | 0.606 | 2.54 |
| `s2_cosy` | 0.99x | 0.0307 | 5.33 |
| `s2_cox` | 3.27x | 0.0122 | 2.94 |
| `s2_cox_cens` | 3.91x | 0.0155 | 3.9 |
| `s2_cumulative_cauchit` | 4.57x | 0.0148 | 2.47 |
| `s2_cumulative_cloglog` | 2.78x | 0.0137 | 2.46 |
| `s2_cumulative_probit` | 1.19x | 0.0186 | 2.5 |
| `s2_custom_vint` | 1.36x | 0.0189 | 2.33 |
| `s2_custom_vreal` | 5.72x | 0.0107 | 2.26 |
| `s2_dirichlet` | 1.04x | 0.0381 | 3.19 |
| `s2_discrete_weibull` | 2.32x | 0.0126 | 2.26 |
| `s2_dist_sigma_re` | 2.26x | 0.0146 | 3.73 |
| `s2_fcor` | 0.904x | 0.0379 | 3.77 |
| `s2_frechet` | 1.5x | 0.0127 | 2.75 |
| `s2_gev` | 0.702x | 0.0261 | 2.59 |
| `s2_gp_approx` | 3.27x | 0.0151 | 4.02 |
| `s2_gp_by_approx` | 3.17x | 0.0202 | 4.81 |
| `s2_gp_by_gr` | 1.35x | 0.0438 | 7.91 |
| `s2_gr_by` | 1.52x | 0.0148 | 3.73 |
| `s2_gr_student` | 1.52x | 0.0144 | 3.97 |
| `s2_hurdle_cumulative` | 1.25x | 0.0262 | 2.96 |
| `s2_hurdle_negbin` | 2.16x | 0.0192 | 2.26 |
| `s2_index_mi` | 1.41x | 0.0145 | 2.99 |
| `s2_logistic_normal` | 1.16x | 0.0577 | 5.24 |
| `s2_me2` | 1.43x | 0.0195 | 5.58 |
| `s2_me2_nomecor` | 1.81x | 0.0156 | 3.19 |
| `s2_mi_lognormal` | 2.14x | 0.0143 | 3.4 |
| `s2_mi_trunc_lb` | 0.948x | 0.0248 | 3.26 |
| `s2_mixture_theta` | 1.31x | 0.0292 | 3.35 |
| `s2_mm` | 1.23x | 0.0166 | 3.46 |
| `s2_mm_weights` | 1.22x | 0.0157 | 3.47 |
| `s2_mmc` | 0.912x | 0.024 | 5.85 |
| `s2_mo_simo_prior` | 2.32x | 0.0148 | 2.78 |
| `s2_multinomial` | 1.14x | 0.0304 | 2.8 |
| `s2_mv_shared_re` | 1.14x | 0.0243 | 6 |
| `s2_mv_subset` | 1.13x | 0.0132 | 2.25 |
| `s2_nl_noloop` | 3.48x | 0.00996 | 2.72 |
| `s2_nlf` | 3.29x | 0.0119 | 2.76 |
| `s2_rate` | 2.37x | 0.0101 | 2.41 |
| `s2_s_by` | 3.38x | 0.0223 | 4.73 |
| `s2_s_cc` | 3.07x | 0.0101 | 3.08 |
| `s2_sar` | 2.63x | 0.0174 | 2.88 |
| `s2_sar_error` | 2.61x | 0.0175 | 2.92 |
| `s2_shifted_lognormal` | 2.71x | 0.0102 | 2.7 |
| `s2_t2_by` | 3.16x | 0.0223 | 4.65 |
| `s2_threading` | 1.28x | 0.01 | 2.17 |
| `s2_unstr` | 1.05x | 0.0431 | 6.44 |
| `s2_weights_trunc` | 1.09x | 0.0225 | 2.42 |
| `s2_wiener` | 1.02x | 0.0707 | 2.5 |
| `s2_zi_asymlaplace` | 0.894x | 0.0266 | 2.41 |
| `s2_zi_beta` | 1.6x | 0.0217 | 2.45 |
| `s2_zoi_beta` | 1.58x | 0.0227 | 2.47 |
| `seeds_centered_model` | 2.12x | 0.00954 | 2.62 |
| `seeds_model` | 1.79x | 0.00993 | 2.41 |
| `seeds_stanified_model` | 1.73x | 0.00926 | 2.34 |
| `sesame_one_pred_a` | 2.43x | 0.00937 | 1.55 |
| `soil_incubation` | 2.16x | 0.0754 | 2.33 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.09x | 0.0241 | 3.68 |
| `surgical_model` | 1.3x | 0.00877 | 2.19 |
| `sw_acat` | 3.55x | 0.107 | 3 |
| `sw_acat_cs` | 3.24x | 0.14 | 4.19 |
| `sw_ar` | 1.68x | 0.0146 | 3.05 |
| `sw_arma` | 1.62x | 0.017 | 3.17 |
| `sw_asymlaplace` | 0.734x | 0.0202 | 2.34 |
| `sw_bernoulli` | 1.17x | 0.00917 | 2.03 |
| `sw_beta` | 1.57x | 0.0133 | 2.68 |
| `sw_binomial` | 1.38x | 0.0122 | 2.35 |
| `sw_categorical` | 1.12x | 0.0117 | 3.12 |
| `sw_cens` | 2.02x | 0.0135 | 3.1 |
| `sw_cratio` | 1.26x | 0.147 | 2.55 |
| `sw_cratio_cs` | 1.39x | 0.186 | 3.74 |
| `sw_cumulative` | 1.01x | 0.0532 | 2.6 |
| `sw_cumulative_cs` | 1.23x | 0.164 | 3.69 |
| `sw_dist_sigma` | 3.36x | 0.012 | 2.63 |
| `sw_exgaussian` | 1.85x | 0.0112 | 2.69 |
| `sw_gamma` | 2.48x | 0.0097 | 2.54 |
| `sw_gaussian` | 1.14x | 0.00985 | 2.09 |
| `sw_gp` | 1.23x | 0.0519 | 5.98 |
| `sw_hurdle_gamma` | 2.29x | 0.0153 | 2.34 |
| `sw_hurdle_lognormal` | 2.62x | 0.0157 | 2.36 |
| `sw_hurdle_pois` | 2.66x | 0.0147 | 2.17 |
| `sw_lognormal` | 2.63x | 0.0101 | 2.5 |
| `sw_ma` | 1.69x | 0.0144 | 3.03 |
| `sw_me` | 1.9x | 0.0136 | 2.91 |
| `sw_mi` | 1.14x | 0.0142 | 2.63 |
| `sw_mixture` | 1.39x | 0.0245 | 3.59 |
| `sw_mono` | 2.22x | 0.0145 | 2.75 |
| `sw_mv_norescor` | 1.15x | 0.0131 | 2.26 |
| `sw_mv_rescor` | 1.11x | 0.0285 | 5.08 |
| `sw_negbinomial` | 1.1x | 0.0111 | 2.17 |
| `sw_nonlinear` | 4.47x | 0.0104 | 2.68 |
| `sw_poisson` | 1.16x | 0.00898 | 1.86 |
| `sw_re_bern` | 1.65x | 0.0134 | 3.47 |
| `sw_re_gauss` | 1.45x | 0.0141 | 3.35 |
| `sw_re_negbin` | 1.26x | 0.0159 | 3.54 |
| `sw_re_pois` | 1.56x | 0.0128 | 3.28 |
| `sw_re_slope` | 1.04x | 0.0194 | 5.68 |
| `sw_se` | 2.71x | 0.00983 | 2.31 |
| `sw_skewnormal` | 1.51x | 0.014 | 2.77 |
| `sw_spline_s` | 3.36x | 0.0111 | 3.25 |
| `sw_spline_t2` | 3.11x | 0.0148 | 3.7 |
| `sw_sratio` | 1.32x | 0.145 | 2.55 |
| `sw_student` | 2.53x | 0.0102 | 2.64 |
| `sw_trunc` | 1.14x | 0.022 | 2.38 |
| `sw_vonmises` | 2.14x | 0.01 | 2.49 |
| `sw_weibull` | 1.49x | 0.0128 | 2.68 |
| `sw_weights` | 3.26x | 0.0108 | 2.29 |
| `sw_zi_binomial` | 1.78x | 0.017 | 2.2 |
| `sw_zi_negbin` | 1.61x | 0.0184 | 2.23 |
| `sw_zi_poisson` | 2.21x | 0.0149 | 2.14 |
| `wells_daae_c_model` | 1.04x | 0.0508 | 2.2 |
| `wells_dae_c_model` | 1.02x | 0.0468 | 2.18 |
| `wells_dae_inter_model` | 0.988x | 0.0534 | 2.16 |
| `wells_dae_model` | 0.989x | 0.0491 | 2.02 |
| `wells_dist` | 1.84x | 0.0513 | 1.75 |
| `wells_dist100_model` | 1.01x | 0.0441 | 2.01 |
| `wells_dist100ars_model` | 1.04x | 0.0448 | 2.01 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.01x | 0.0505 | 2.13 |
| `wells_interaction_model` | 1.01x | 0.0486 | 2.07 |

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
| `2pl_latent_reg_irt` | 1.59x | 0.151 | 4.45 |
| `GLMM1_model` | 3.23x | 0.0319 | 2.04 |
| `GLMM_Poisson_model` | 1.88x | 0.00897 | 2.59 |
| `GLM_Binomial_model` | 1.33x | 0.0088 | 2.01 |
| `GLM_Poisson_model` | 2.05x | 0.00835 | 2.03 |
| `M0_model` | 14.8x | 0.0123 | 1.44 |
| `Mb_model` | 1.24x | 0.126 | 2.11 |
| `Mh_model` | 2.27x | 0.0464 | 2.1 |
| `Mt_model` | 16.5x | 0.0127 | 2.27 |
| `Mtbh_model` | 3.17x | 0.0532 | 3.77 |
| `Mth_model` | 4.67x | 0.0684 | 3.28 |
| `Rate_1_model` | 1.34x | 0.00588 | 1.12 |
| `Rate_2_model` | 1.35x | 0.00665 | 1.26 |
| `Rate_3_model` | 1.2x | 0.00614 | 1.15 |
| `Rate_4_model` | 1.45x | 0.00663 | 1.24 |
| `Rate_5_model` | 1.32x | 0.00616 | 1.24 |
| `Survey_model` | 1.13x | 0.128 | 1.84 |
| `aalto_bern` | 1.44x | 0.00669 | 1.28 |
| `aalto_binom` | 1.39x | 0.00608 | 1.12 |
| `aalto_binom2` | 1.33x | 0.00683 | 1.25 |
| `aalto_binomb` | 1.36x | 0.00641 | 1.15 |
| `aalto_gpareto` | 0.724x | 0.0142 | 1.81 |
| `aalto_grp_aov` | 1.36x | 0.00737 | 1.78 |
| `aalto_grp_prior_mean` | 1.31x | 0.00746 | 1.87 |
| `aalto_grp_prior_mean_var` | 1.33x | 0.00851 | 2.6 |
| `aalto_lin` | 1.62x | 0.0082 | 1.76 |
| `aalto_lin_std` | 1.56x | 0.00855 | 1.97 |
| `aalto_lin_std_t` | 1.49x | 0.00938 | 2.21 |
| `aalto_poisson_hurdle` | 25.7x | 0.0272 | 1.79 |
| `aalto_poisson_simple` | 1.03x | 0.00928 | 1.38 |
| `accel_gp` | 1.76x | 0.0314 | 5.34 |
| `accel_splines` | 1.57x | 0.0239 | 3.36 |
| `arK` | 4.84x | 0.0124 | 1.62 |
| `arma11` | 0.829x | 0.0175 | 1.71 |
| `blr` | 0.827x | 0.00852 | 1.81 |
| `bones_model` | 1.31x | 0.105 | 2.37 |
| `bym2_offset_only` | 1.69x | 0.0858 | 3.26 |
| `ch09_m5_8s` | 2x | 0.00865 | 1.78 |
| `ch09_m5_8s2` | 1.95x | 0.00869 | 1.76 |
| `ch09_m9_1` | 2.05x | 0.00967 | 2.03 |
| `ch09_m9_1_chains4` | 2.1x | 0.00973 | 2.05 |
| `ch09_m9_2` | 1.46x | 0.00638 | 1.25 |
| `ch09_m9_3` | 1.43x | 0.0062 | 1.25 |
| `ch09_m9_4` | 1.33x | 0.00712 | 1.29 |
| `ch09_m9_5` | 1.28x | 0.007 | 1.29 |
| `ch09_mp` | 2x | 0.00609 | 1.03 |
| `ch11_m11_10` | 1.32x | 0.00846 | 2.06 |
| `ch11_m11_11` | 1.64x | 0.00858 | 2.32 |
| `ch11_m11_4` | 1.72x | 0.0262 | 1.78 |
| `ch11_m11_5` | 2.19x | 0.0273 | 1.86 |
| `ch11_m11_6` | 1.26x | 0.00897 | 1.77 |
| `ch11_m11_7` | 1.39x | 0.00698 | 1.59 |
| `ch11_m11_8` | 1.32x | 0.00757 | 1.67 |
| `ch11_m11_9` | 1.54x | 0.00687 | 1.33 |
| `ch11_m_pois` | 1.44x | 0.00668 | 1.25 |
| `ch12_m12_1` | 1.25x | 0.00897 | 1.94 |
| `ch12_m12_2` | 1.27x | 0.00967 | 2.76 |
| `ch12_m12_3` | 17.2x | 0.0109 | 1.28 |
| `ch12_m12_3_alt` | 5.6x | 0.015 | 1.28 |
| `ch12_m12_4` | 105x | 0.0705 | 7.44 |
| `ch12_m12_5` | 1.13x | 5.54 | 8.53 |
| `ch12_m12_6` | 1.02x | 6.19 | 9.28 |
| `ch12_m12_7` | 1.12x | 5.34 | 8.22 |
| `ch13_m13_1` | 1.25x | 0.00944 | 1.7 |
| `ch13_m13_2` | 1.3x | 0.0101 | 1.88 |
| `ch13_m13_3` | 1.3x | 0.00966 | 1.77 |
| `ch13_m13_4` | 1.69x | 0.0325 | 2.19 |
| `ch13_m13_4b` | 1.72x | 0.0285 | 2.08 |
| `ch13_m13_4nc` | 2.1x | 0.0287 | 2.08 |
| `ch13_m13_5` | 1.77x | 0.0269 | 2.01 |
| `ch13_m13_6` | 1.69x | 0.0326 | 2.18 |
| `ch13_m13_7` | 1.89x | 0.0058 | 1.06 |
| `ch13_m13_7nc` | 1.99x | 0.00627 | 1.1 |
| `ch14_m14_1` | 1.13x | 0.0222 | 5.72 |
| `ch14_m14_10` | 1.28x | 0.743 | 3.95 |
| `ch14_m14_11` | 1.15x | 1.22 | 4.58 |
| `ch14_m14_2` | 1.2x | 0.0522 | 5.97 |
| `ch14_m14_3` | 1.83x | 0.0413 | 4.75 |
| `ch14_m14_4` | 1.76x | 0.0108 | 1.62 |
| `ch14_m14_4x` | 1.82x | 0.0108 | 1.61 |
| `ch14_m14_5` | 2.07x | 0.0127 | 1.77 |
| `ch14_m14_6` | 1.21x | 0.221 | 5.83 |
| `ch14_m14_6x` | 1.26x | 0.216 | 5.85 |
| `ch14_m14_7` | 1.39x | 0.0644 | 7.73 |
| `ch14_m14_8` | 1.3x | 0.0147 | 3.55 |
| `ch14_m14_8nc` | 1.37x | 0.0165 | 4.79 |
| `ch14_m14_9` | 1.25x | 0.7 | 3.89 |
| `ch15_m15_1` | 1.87x | 0.00881 | 2.16 |
| `ch15_m15_2` | 1.54x | 0.0101 | 2.41 |
| `ch15_m15_3` | 0.967x | 0.0564 | 1.41 |
| `ch15_m15_4` | 0.938x | 0.0488 | 1.41 |
| `ch15_m15_5` | 2.17x | 0.00959 | 2.44 |
| `ch15_m15_6` | 1.62x | 0.00796 | 1.9 |
| `ch15_m15_7` | 1.36x | 0.0316 | 6.44 |
| `ch15_m15_8` | 1.89x | 0.0143 | 1.6 |
| `ch15_m15_9` | 1.84x | 0.0166 | 1.88 |
| `ch16_m16_1` | 2.34x | 0.0349 | 1.81 |
| `ch16_m16_4` | 1.94x | 0.0108 | 2.11 |
| `covid19imperial_v2` | 1.48x | 1.42 | 6.28 |
| `covid19imperial_v3` | 1.43x | 1.43 | 6.28 |
| `diamonds` | 0.955x | 0.0924 | 2.26 |
| `dogs` | 4.68x | 0.0388 | 2.99 |
| `dogs_hierarchical` | 3.22x | 0.0436 | 1.58 |
| `dogs_nonhierarchical` | 2.62x | 0.0571 | 5.75 |
| `dugongs_model` | 1.83x | 0.00842 | 1.76 |
| `earn_height` | 1.73x | 0.016 | 1.6 |
| `eight_schools_centered` | 1.39x | 0.00762 | 1.72 |
| <code>eight_schools_</code><br><code>noncentered</code> | 1.38x | 0.0072 | 1.97 |
| `election88_full` | 2.09x | 0.461 | 4.78 |
| `extra_hurdle_poisson` | 2.16x | 0.00715 | 1.33 |
| `garch11` | 0.937x | 0.0234 | 1.69 |
| `gp_pois_regr` | 1.33x | 0.0117 | 4.44 |
| `gp_regr` | 1.33x | 0.0116 | 4.25 |
| `gpcm_latent_reg_irt` | 15.6x | 0.336 | 7.92 |
| `grsm_latent_reg_irt` | 13.7x | 0.177 | 6.05 |
| `hier_2pl` | 1.58x | 0.422 | 6.27 |
| `hierarchical_gp` | 2.36x | 0.0676 | 7.97 |
| `hmm_drive_0` | 1.14x | 0.292 | 3.86 |
| `hmm_drive_1` | 1.23x | 0.3 | 3.85 |
| `hmm_example` | 1.6x | 0.0531 | 2.91 |
| `hmm_gaussian` | 1.39x | 0.574 | 4.17 |
| `i319_gauss_re` | 1.19x | 0.0213 | 3.4 |
| `i319_negbin_fixed` | 0.964x | 0.0259 | 2.2 |
| `i319_negbin_re` | 1.18x | 0.0349 | 3.6 |
| `i319_pois_fixed` | 1x | 0.0133 | 1.9 |
| `i319_pois_re` | 1.33x | 0.0236 | 3.33 |
| `i319_pois_re2` | 1.44x | 0.0315 | 3.65 |
| `i320_gp_expquad` | 1.25x | 0.0344 | 6.15 |
| `i320_gp_matern32` | 0.872x | 0.0765 | 6.1 |
| `i320_mi_nhanes` | 1.22x | 0.0187 | 3.23 |
| `i320_pois_trunc_both` | 0.987x | 0.0741 | 2.38 |
| `i320_pois_trunc_ub` | 1.06x | 0.0582 | 2.25 |
| `i320_sratio_cs` | 1.81x | 0.19 | 4.44 |
| `i320_sratio_plain` | 1.2x | 0.145 | 2.96 |
| `iohmm_reg` | 1.85x | 0.599 | 5.71 |
| `irt_2pl` | 1.26x | 0.0445 | 3.02 |
| `kidscore_interaction` | 2.24x | 0.0127 | 1.94 |
| `kidscore_interaction_c` | 2.29x | 0.0126 | 1.95 |
| `kidscore_interaction_c2` | 2.26x | 0.0125 | 1.91 |
| `kidscore_interaction_z` | 2.26x | 0.0126 | 1.99 |
| `kidscore_mom_work` | 2.2x | 0.0132 | 1.84 |
| `kidscore_momhs` | 1.71x | 0.0101 | 1.64 |
| `kidscore_momhsiq` | 2.15x | 0.0116 | 1.81 |
| `kidscore_momiq` | 1.68x | 0.01 | 1.66 |
| `kilpisjarvi` | 1.63x | 0.00801 | 1.54 |
| `ldaK2` | 2.36x | 0.118 | 2.66 |
| `ldaK5` | 2.51x | 5.39 | 14.7 |
| `log10earn_height` | 1.76x | 0.0165 | 1.64 |
| `logearn_height` | 1.66x | 0.0161 | 1.64 |
| `logearn_height_male` | 2.09x | 0.0197 | 1.82 |
| `logearn_interaction` | 2.18x | 0.0234 | 1.95 |
| `logearn_interaction_z` | 2.19x | 0.0231 | 2.07 |
| `logearn_logheight_male` | 2.12x | 0.0191 | 1.82 |
| `logistic_regression_rhs` | 1.19x | 0.104 | 3.93 |
| `logmesquite` | 2.89x | 0.00982 | 2.15 |
| `logmesquite_logva` | 2.29x | 0.00911 | 2.06 |
| `logmesquite_logvas` | 2.79x | 0.0099 | 2.39 |
| `logmesquite_logvash` | 2.68x | 0.00957 | 2.3 |
| `logmesquite_logvolume` | 1.76x | 0.00817 | 1.77 |
| `losscurve_sislob` | 1.92x | 0.0161 | 3.49 |
| `lotka_volterra` | 2.02x | 0.0569 | 3.32 |
| `low_dim_gauss_mix` | 2.02x | 0.114 | 2.16 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.09x | 0.108 | 2.06 |
| `lsat_model` | 1.62x | 0.0893 | 2.63 |
| `mesquite` | 2.89x | 0.00933 | 2.09 |
| `multi_occupancy` | 2.27x | 0.0749 | 5.42 |
| `nes` | 2.66x | 0.0463 | 2.44 |
| `nes_logit_model` | 1.03x | 0.0198 | 1.94 |
| `nn_rbm1bJ10` | 1.2x | 0.344 | 4.62 |
| `nn_rbm1bJ100` | 1.07x | 860 | 927 |
| `normal_mixture` | 2.08x | 0.101 | 1.75 |
| `normal_mixture_k` | 1.93x | 0.452 | 2.99 |
| `one_comp_mm_elim_abs` | 1.03x | 1.05 | 3.62 |
| `pilots` | 1.39x | 0.0103 | 2.24 |
| `prophet` | 1.49x | 0.094 | 4.46 |
| `radon_county` | 2.1x | 0.0864 | 2.03 |
| `radon_county_intercept` | 6.8x | 0.156 | 2.44 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 6.83x | 0.215 | 2.97 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 6.98x | 0.216 | 3.14 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 7.93x | 0.115 | 2.35 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 7.66x | 0.116 | 2.54 |
| `radon_pooled` | 1.82x | 0.104 | 1.76 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 6.96x | 0.156 | 2.54 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 6.99x | 0.158 | 2.77 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 6.2x | 0.178 | 2.74 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 6.21x | 0.178 | 3.01 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 6.86x | 0.156 | 2.57 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 6.63x | 0.164 | 2.77 |
| `rats_model` | 4.07x | 0.0112 | 1.82 |
| `s2_ar_cov` | 1.02x | 0.0323 | 6.41 |
| `s2_beta_binomial` | 1.12x | 0.0175 | 2.47 |
| `s2_car` | 2x | 0.0145 | 3.5 |
| `s2_car_esicar` | 1.98x | 0.0145 | 3.57 |
| `s2_car_icar` | 2.37x | 0.0131 | 3.17 |
| `s2_categorical_re` | 1.47x | 0.0289 | 4.22 |
| `s2_cens_interval` | 1.78x | 0.014 | 2.52 |
| `s2_com_poisson` | 0.165x | 0.609 | 3.53 |
| `s2_cosy` | 0.97x | 0.0312 | 5.7 |
| `s2_cox` | 3.24x | 0.0132 | 2.85 |
| `s2_cox_cens` | 3.98x | 0.0156 | 3.79 |
| `s2_cumulative_cauchit` | 3.9x | 0.0143 | 2.55 |
| `s2_cumulative_cloglog` | 2.31x | 0.0132 | 2.56 |
| `s2_cumulative_probit` | 1.35x | 0.0188 | 2.46 |
| `s2_custom_vint` | 1.37x | 0.0189 | 2.31 |
| `s2_custom_vreal` | 5.37x | 0.0112 | 2.22 |
| `s2_dirichlet` | 1.05x | 0.0383 | 3.16 |
| `s2_discrete_weibull` | 2.06x | 0.0133 | 2.28 |
| `s2_dist_sigma_re` | 1.87x | 0.0144 | 3.78 |
| `s2_fcor` | 0.947x | 0.0377 | 3.72 |
| `s2_frechet` | 1.15x | 0.0134 | 2.73 |
| `s2_gev` | 0.601x | 0.0266 | 2.65 |
| `s2_gp_approx` | 2.33x | 0.014 | 4.3 |
| `s2_gp_by_approx` | 3.35x | 0.0215 | 5.33 |
| `s2_gr_by` | 1.49x | 0.0146 | 3.76 |
| `s2_gr_student` | 1.48x | 0.0156 | 4.15 |
| `s2_hurdle_cumulative` | 1.25x | 0.0265 | 2.94 |
| `s2_hurdle_negbin` | 2.12x | 0.0196 | 2.29 |
| `s2_index_mi` | 1.27x | 0.0145 | 2.98 |
| `s2_logistic_normal` | 1.06x | 0.056 | 5.61 |
| `s2_me2` | 1.39x | 0.0192 | 5.54 |
| `s2_me2_nomecor` | 1.63x | 0.0166 | 3.33 |
| `s2_mi_lognormal` | 1.68x | 0.0137 | 3.37 |
| `s2_mi_trunc_lb` | 0.917x | 0.0252 | 3.22 |
| `s2_mixture_theta` | 1.23x | 0.0296 | 3.23 |
| `s2_mm` | 1.2x | 0.0159 | 3.51 |
| `s2_mm_weights` | 1.22x | 0.0158 | 3.5 |
| `s2_mmc` | 0.876x | 0.0244 | 5.89 |
| `s2_mo_simo_prior` | 1.85x | 0.0148 | 2.91 |
| `s2_multinomial` | 1.13x | 0.032 | 2.76 |
| `s2_mv_shared_re` | 1.12x | 0.0234 | 5.98 |
| `s2_mv_subset` | 1.08x | 0.0131 | 2.27 |
| `s2_nl_noloop` | 1.62x | 0.00978 | 2.61 |
| `s2_nlf` | 2.23x | 0.0121 | 3.14 |
| `s2_rate` | 1.46x | 0.0102 | 2.36 |
| `s2_s_by` | 2.09x | 0.0231 | 4.5 |
| `s2_s_cc` | 2.03x | 0.0107 | 3 |
| `s2_sar` | 2.25x | 0.0175 | 2.99 |
| `s2_sar_error` | 2.22x | 0.0189 | 2.99 |
| `s2_shifted_lognormal` | 2.02x | 0.0103 | 2.7 |
| `s2_t2_by` | 1.77x | 0.0227 | 4.46 |
| `s2_threading` | 1.19x | 0.00996 | 2.17 |
| `s2_unstr` | 1.1x | 0.0429 | 7.62 |
| `s2_weights_trunc` | 1.15x | 0.0217 | 2.37 |
| `s2_wiener` | 1.03x | 0.0698 | 2.49 |
| `s2_zi_asymlaplace` | 0.892x | 0.0254 | 2.47 |
| `s2_zi_beta` | 1.43x | 0.0206 | 2.51 |
| `s2_zoi_beta` | 1.41x | 0.0229 | 2.57 |
| `seeds_centered_model` | 1.87x | 0.00992 | 2.66 |
| `seeds_model` | 1.56x | 0.00955 | 2.48 |
| `seeds_stanified_model` | 1.44x | 0.00998 | 2.38 |
| `sesame_one_pred_a` | 1.8x | 0.00931 | 1.58 |
| `soil_incubation` | 2.13x | 0.0713 | 2.81 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3x | 0.0237 | 3.68 |
| `surgical_model` | 1.3x | 0.00807 | 2.18 |
| `sw_acat` | 3.06x | 0.113 | 3.01 |
| `sw_acat_cs` | 3.45x | 0.142 | 4.27 |
| `sw_ar` | 1.54x | 0.0146 | 3.04 |
| `sw_arma` | 1.39x | 0.018 | 3.14 |
| `sw_asymlaplace` | 0.758x | 0.0209 | 2.41 |
| `sw_bernoulli` | 1.16x | 0.00871 | 2.03 |
| `sw_beta` | 1.19x | 0.0132 | 2.69 |
| `sw_binomial` | 1.13x | 0.0122 | 2.26 |
| `sw_categorical` | 1.12x | 0.0129 | 3.16 |
| `sw_cens` | 1.58x | 0.0135 | 3.02 |
| `sw_cratio` | 1.18x | 0.142 | 2.83 |
| `sw_cratio_cs` | 1.78x | 0.19 | 4.12 |
| `sw_cumulative` | 0.975x | 0.0537 | 2.57 |
| `sw_cumulative_cs` | 1.79x | 0.16 | 3.99 |
| `sw_dist_sigma` | 1.82x | 0.0123 | 2.55 |
| `sw_exgaussian` | 1.34x | 0.0117 | 2.65 |
| `sw_gamma` | 1.33x | 0.0104 | 2.53 |
| `sw_gaussian` | 1.1x | 0.00999 | 2.07 |
| `sw_gp` | 1.19x | 0.0525 | 6.18 |
| `sw_hurdle_gamma` | 2.18x | 0.0162 | 2.32 |
| `sw_hurdle_lognormal` | 2.44x | 0.0144 | 2.34 |
| `sw_hurdle_pois` | 2.48x | 0.0155 | 2.19 |
| `sw_lognormal` | 1.66x | 0.0103 | 2.44 |
| `sw_ma` | 1.53x | 0.0145 | 3.01 |
| `sw_me` | 1.67x | 0.0127 | 3.06 |
| `sw_mi` | 1.09x | 0.0133 | 2.64 |
| `sw_mixture` | 1.41x | 0.0237 | 3.55 |
| `sw_mono` | 1.86x | 0.0146 | 2.87 |
| `sw_mv_norescor` | 1.08x | 0.0133 | 2.27 |
| `sw_mv_rescor` | 1.13x | 0.0277 | 4.99 |
| `sw_negbinomial` | 1.03x | 0.011 | 2.18 |
| `sw_nonlinear` | 1.74x | 0.0105 | 2.67 |
| `sw_poisson` | 1.1x | 0.00968 | 1.88 |
| `sw_re_bern` | 1.61x | 0.0125 | 3.45 |
| `sw_re_gauss` | 1.41x | 0.0133 | 3.39 |
| `sw_re_negbin` | 1.32x | 0.0153 | 3.57 |
| `sw_re_pois` | 1.58x | 0.0137 | 3.31 |
| `sw_re_slope` | 1.06x | 0.0197 | 5.71 |
| `sw_se` | 1.66x | 0.0092 | 2.2 |
| `sw_skewnormal` | 1.39x | 0.0142 | 2.7 |
| `sw_spline_s` | 2.08x | 0.012 | 3.14 |
| `sw_spline_t2` | 1.77x | 0.0144 | 3.61 |
| `sw_sratio` | 1.17x | 0.143 | 2.83 |
| `sw_student` | 1.75x | 0.0109 | 2.56 |
| `sw_trunc` | 1.12x | 0.0213 | 2.33 |
| `sw_vonmises` | 2.26x | 0.011 | 2.43 |
| `sw_weibull` | 1.13x | 0.0126 | 2.67 |
| `sw_weights` | 3.25x | 0.0114 | 2.23 |
| `sw_zi_binomial` | 1.77x | 0.017 | 2.22 |
| `sw_zi_negbin` | 1.57x | 0.0193 | 2.25 |
| `sw_zi_poisson` | 2.03x | 0.015 | 2.18 |
| `wells_daae_c_model` | 1.06x | 0.0505 | 2.22 |
| `wells_dae_c_model` | 1.03x | 0.0477 | 2.19 |
| `wells_dae_inter_model` | 0.997x | 0.0526 | 2.17 |
| `wells_dae_model` | 1.05x | 0.0487 | 2.05 |
| `wells_dist` | 1.53x | 0.0516 | 1.79 |
| `wells_dist100_model` | 1.01x | 0.0431 | 1.99 |
| `wells_dist100ars_model` | 1.02x | 0.0455 | 2.03 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.04x | 0.0499 | 2.14 |
| `wells_interaction_model` | 0.982x | 0.0505 | 2.08 |

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
