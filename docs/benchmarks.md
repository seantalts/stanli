# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling, it reuses a prepared autodiff graph and batches
independent work. This can shorten the first fit and repeated gradient
evaluations; the gain depends on the model.

The current comparison covers **<!--gen:benchmark_models-->342<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->336<!--/gen--> accepted gradient
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
Complete results are sorted by CmdStan / Stanli gradient ratio, highest first.

<!--gen:benchmark_catalog-->
Run `6c7cf86a5b24c3d8` (2026-10-10): 342 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 32.5x | 0.152 | 7.46 |
| `gpcm_latent_reg_irt` | 12.2x | 0.346 | 8.16 |
| `grsm_latent_reg_irt` | 12x | 0.181 | 6.78 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.3x | 0.0915 | 3.58 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 0.095 | 3.82 |
| `radon_county_intercept` | 10.2x | 0.143 | 3.92 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 10.2x | 0.144 | 3.98 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 0.145 | 4 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10x | 0.146 | 4.21 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.77x | 0.147 | 4.2 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.25x | 0.159 | 4.43 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.25x | 0.219 | 4.51 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.2x | 0.216 | 4.61 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.06x | 0.16 | 4.14 |
| `radon_pooled` | 8.81x | 0.0919 | 3.42 |
| `dogs` | 8.2x | 0.0419 | 3.76 |
| `aalto_poisson_hurdle` | 7.83x | 0.039 | 3.09 |
| `s2_custom_vreal` | 7.75x | 0.00959 | 3.48 |
| `rats_model` | 7.45x | 0.0107 | 3.09 |
| `arK` | 7.23x | 0.0122 | 2.91 |
| `ch12_m12_3` | 7.11x | 0.0146 | 2.53 |
| `ch12_m12_3_alt` | 6.93x | 0.0144 | 2.54 |
| `M0_model` | 6.54x | 0.0146 | 2.74 |
| `Mt_model` | 5.52x | 0.0169 | 3.47 |
| `sw_nonlinear` | 5.36x | 0.0102 | 3.89 |
| `ch09_m5_8s` | 4.84x | 0.00792 | 2.86 |
| `s2_cumulative_cauchit` | 4.61x | 0.0161 | 3.68 |
| `ch09_m5_8s2` | 4.53x | 0.00831 | 2.87 |
| `ch14_m14_5` | 4.47x | 0.0122 | 2.86 |
| `ch15_m15_1` | 4.28x | 0.00901 | 3.21 |
| `logmesquite_logvas` | 4.25x | 0.00912 | 3.42 |
| `logmesquite` | 4.25x | 0.00982 | 3.17 |
| `mesquite` | 4.2x | 0.00896 | 3.1 |
| `s2_nl_noloop` | 4.18x | 0.00974 | 3.94 |
| `logmesquite_logvash` | 4.08x | 0.0094 | 3.35 |
| `election88_full` | 4.05x | 0.495 | 6.04 |
| `nes` | 4.02x | 0.0482 | 3.39 |
| `logmesquite_logva` | 3.99x | 0.00816 | 3.16 |
| `ch15_m15_2` | 3.98x | 0.00996 | 3.44 |
| `ch14_m14_4x` | 3.89x | 0.0105 | 2.82 |
| `sw_dist_sigma` | 3.88x | 0.0116 | 3.86 |
| `logmesquite_logvolume` | 3.87x | 0.00754 | 3 |
| `Mth_model` | 3.73x | 0.0676 | 4.48 |
| `ch14_m14_4` | 3.72x | 0.0108 | 2.82 |
| `sw_spline_s` | 3.71x | 0.0111 | 4.46 |
| `sw_weights` | 3.68x | 0.0108 | 3.51 |
| `kidscore_interaction_c2` | 3.65x | 0.0128 | 3.03 |
| `s2_nlf` | 3.64x | 0.011 | 3.96 |
| `kidscore_interaction_c` | 3.64x | 0.0124 | 3.04 |
| `s2_s_cc` | 3.58x | 0.01 | 4.32 |
| `kidscore_interaction` | 3.57x | 0.0128 | 3.04 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.56x | 0.0224 | 4.9 |
| `kidscore_interaction_z` | 3.53x | 0.013 | 3.12 |
| `ch09_m9_1` | 3.51x | 0.00904 | 3.08 |
| `aalto_grp_aov` | 3.49x | 0.0072 | 3.08 |
| `logearn_height_male` | 3.48x | 0.0185 | 2.96 |
| `ch09_m9_1_chains4` | 3.48x | 0.00845 | 3.09 |
| `kidscore_mom_work` | 3.43x | 0.0135 | 2.95 |
| `kilpisjarvi` | 3.41x | 0.00731 | 2.81 |
| `logearn_interaction` | 3.39x | 0.0229 | 3.06 |
| `GLMM1_model` | 3.36x | 0.0315 | 3.29 |
| `s2_gp_approx` | 3.34x | 0.0146 | 5.22 |
| `logearn_interaction_z` | 3.32x | 0.0247 | 3.19 |
| `s2_shifted_lognormal` | 3.32x | 0.01 | 3.92 |
| `s2_cox_cens` | 3.31x | 0.0147 | 5.07 |
| `sw_se` | 3.3x | 0.00968 | 3.51 |
| `sw_lognormal` | 3.29x | 0.00982 | 3.7 |
| `logearn_logheight_male` | 3.29x | 0.0193 | 2.97 |
| `sw_spline_t2` | 3.27x | 0.0139 | 4.93 |
| `s2_t2_by` | 3.26x | 0.0205 | 5.89 |
| `s2_s_by` | 3.26x | 0.0219 | 5.86 |
| `ch16_m16_4` | 3.22x | 0.0104 | 2.91 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.2x | 0.0074 | 3.28 |
| `kidscore_momhsiq` | 3.17x | 0.0112 | 2.96 |
| `s2_gp_by_approx` | 3.15x | 0.0209 | 6 |
| `radon_county` | 3.14x | 0.0647 | 3.3 |
| `s2_cox` | 3.08x | 0.0132 | 4.18 |
| `ch15_m15_5` | 2.97x | 0.0091 | 3.25 |
| `sw_acat` | 2.96x | 0.112 | 4.16 |
| `aalto_grp_prior_mean` | 2.93x | 0.00708 | 3.17 |
| `s2_cumulative_cloglog` | 2.91x | 0.013 | 3.68 |
| `Mtbh_model` | 2.89x | 0.0556 | 5.09 |
| `s2_hurdle_cumulative` | 2.89x | 0.0184 | 4.17 |
| `logearn_height` | 2.88x | 0.0144 | 2.87 |
| `log10earn_height` | 2.88x | 0.0151 | 2.83 |
| `sesame_one_pred_a` | 2.87x | 0.0091 | 2.8 |
| `ch15_m15_6` | 2.85x | 0.00774 | 3.01 |
| `bym2_offset_only` | 2.84x | 0.0862 | 4.42 |
| `pilots` | 2.8x | 0.00968 | 3.4 |
| `GLM_Poisson_model` | 2.78x | 0.00872 | 3.29 |
| `earn_height` | 2.77x | 0.0145 | 2.82 |
| `sw_hurdle_lognormal` | 2.76x | 0.0143 | 3.59 |
| `cm_geg` | 2.75x | 0.0653 | 3.99 |
| `sw_student` | 2.75x | 0.0107 | 3.89 |
| `kidscore_momhs` | 2.74x | 0.00966 | 2.87 |
| `GLMM_Poisson_model` | 2.71x | 0.00902 | 3.85 |
| `blr` | 2.71x | 0.00771 | 3.12 |
| `s2_mi_lognormal` | 2.7x | 0.0138 | 4.57 |
| `dogs_nonhierarchical` | 2.69x | 0.0595 | 7 |
| `eight_schools_centered` | 2.69x | 0.00701 | 3 |
| `ch12_m12_5` | 2.66x | 2.05 | 8.92 |
| `sw_gamma` | 2.66x | 0.0103 | 3.79 |
| `ch12_m12_7` | 2.65x | 1.99 | 8.58 |
| `kidscore_momiq` | 2.65x | 0.00965 | 2.88 |
| `hierarchical_gp` | 2.58x | 0.0641 | 9.16 |
| `aalto_lin_std` | 2.57x | 0.00827 | 3.32 |
| `sw_hurdle_pois` | 2.55x | 0.0154 | 3.4 |
| `s2_sar_error` | 2.54x | 0.0188 | 4.11 |
| `cm_exgaussian` | 2.54x | 0.0445 | 3.73 |
| `aalto_grp_prior_mean_var` | 2.51x | 0.0085 | 3.92 |
| `sw_acat_cs` | 2.51x | 0.157 | 5.36 |
| `aalto_lin` | 2.51x | 0.00764 | 3.09 |
| `ldaK5` | 2.51x | 5.19 | 15.6 |
| `Mh_model` | 2.49x | 0.047 | 3.38 |
| `losscurve_sislob` | 2.48x | 0.0153 | 4.33 |
| `s2_sar` | 2.44x | 0.0177 | 4.07 |
| `s2_rate` | 2.37x | 0.00932 | 3.63 |
| `seeds_centered_model` | 2.31x | 0.0094 | 3.89 |
| `logistic_regression_rhs` | 2.3x | 0.107 | 5.09 |
| `sw_vonmises` | 2.29x | 0.00988 | 3.7 |
| `extra_hurdle_poisson` | 2.27x | 0.00678 | 2.59 |
| `ch09_m9_4` | 2.24x | 0.00654 | 2.57 |
| `dugongs_model` | 2.23x | 0.00864 | 3.04 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.21x | 0.102 | 3.33 |
| `aalto_lin_std_t` | 2.2x | 0.00928 | 3.51 |
| `lsat_model` | 2.19x | 0.0944 | 3.91 |
| `ch09_m9_5` | 2.18x | 0.00679 | 2.55 |
| `dogs_hierarchical` | 2.16x | 0.0568 | 2.85 |
| `s2_gev` | 2.16x | 0.0166 | 3.78 |
| `normal_mixture` | 2.15x | 0.0965 | 2.79 |
| `2pl_latent_reg_irt` | 2.12x | 0.18 | 6.28 |
| `low_dim_gauss_mix` | 2.12x | 0.108 | 3.43 |
| `ldaK2` | 2.1x | 0.12 | 3.93 |
| `ch15_m15_9` | 2.1x | 0.0174 | 3.03 |
| `sw_zi_poisson` | 2.09x | 0.0148 | 3.38 |
| `hier_2pl` | 2.09x | 0.409 | 7.66 |
| `ch09_m9_2` | 2.08x | 0.00612 | 2.52 |
| `s2_car_icar` | 2.06x | 0.013 | 4.43 |
| `multi_occupancy` | 2.04x | 0.0842 | 6.16 |
| `irt_2pl` | 2.03x | 0.042 | 4.2 |
| `ch09_m9_3` | 2.02x | 0.00657 | 2.54 |
| `accel_gp` | 2.02x | 0.031 | 5.44 |
| `sw_me` | 2.01x | 0.0123 | 4.14 |
| `ch15_m15_8` | 2x | 0.0158 | 2.88 |
| `prophet` | 1.98x | 0.0904 | 5.09 |
| `seeds_stanified_model` | 1.97x | 0.00942 | 3.56 |
| `seeds_model` | 1.96x | 0.00989 | 3.7 |
| `soil_incubation` | 1.94x | 0.0747 | 3.58 |
| `accel_splines` | 1.92x | 0.0237 | 4.44 |
| `normal_mixture_k` | 1.92x | 0.442 | 4.21 |
| `ch13_m13_7nc` | 1.91x | 0.00625 | 2.35 |
| `sw_exgaussian` | 1.91x | 0.012 | 3.92 |
| `ch13_m13_7` | 1.89x | 0.00612 | 2.32 |
| `sw_mi` | 1.87x | 0.0134 | 3.85 |
| `sw_zi_binomial` | 1.87x | 0.0173 | 3.42 |
| `lotka_volterra` | 1.86x | 0.0569 | 4.43 |
| `sw_ma` | 1.85x | 0.015 | 4.26 |
| `s2_car` | 1.84x | 0.0159 | 4.39 |
| `s2_cens_interval` | 1.84x | 0.013 | 3.77 |
| `s2_car_esicar` | 1.83x | 0.0141 | 4.51 |
| `sw_hurdle_gamma` | 1.78x | 0.0175 | 3.56 |
| `ch09_mp` | 1.78x | 0.00638 | 2.31 |
| `s2_dist_sigma_re` | 1.77x | 0.0148 | 4.9 |
| `sw_ar` | 1.76x | 0.0136 | 4.25 |
| `s2_threading` | 1.76x | 0.00975 | 3.39 |
| `s2_index_mi` | 1.75x | 0.0145 | 4.21 |
| `s2_mv_subset` | 1.73x | 0.0118 | 3.47 |
| `ch11_m11_11` | 1.73x | 0.00884 | 3.56 |
| `sw_beta` | 1.71x | 0.0128 | 3.91 |
| `wells_dist` | 1.68x | 0.0573 | 3.03 |
| `sw_asymlaplace` | 1.66x | 0.0152 | 3.56 |
| `sw_arma` | 1.64x | 0.018 | 4.36 |
| `cm_choco` | 1.64x | 0.116 | 4.63 |
| `sw_zi_negbin` | 1.64x | 0.0192 | 3.48 |
| `s2_zi_beta` | 1.63x | 0.0215 | 3.67 |
| `cm_logweibull` | 1.62x | 0.0369 | 4.05 |
| `cm_invgamma` | 1.62x | 0.0396 | 4.08 |
| `cm_weibull` | 1.61x | 0.0377 | 4.08 |
| `sw_mv_norescor` | 1.6x | 0.0123 | 3.46 |
| `GLM_Binomial_model` | 1.6x | 0.00884 | 3.28 |
| `sw_gaussian` | 1.6x | 0.00942 | 3.29 |
| `cm_logstudent` | 1.59x | 0.0405 | 4.19 |
| `cm_betagate` | 1.59x | 0.0844 | 4.08 |
| `cm_invweibull` | 1.59x | 0.0401 | 4.06 |
| `surgical_model` | 1.58x | 0.00805 | 3.42 |
| `cm_loggamma` | 1.57x | 0.0456 | 4.22 |
| `cm_bisa` | 1.55x | 0.0389 | 4.06 |
| `cm_lba1` | 1.55x | 0.0627 | 4.39 |
| `s2_zoi_beta` | 1.55x | 0.0216 | 3.7 |
| `cm_lognormal` | 1.54x | 0.0752 | 4.27 |
| `sw_weibull` | 1.54x | 0.0128 | 3.9 |
| `s2_unstr` | 1.53x | 0.0372 | 7.48 |
| `gp_pois_regr` | 1.52x | 0.0111 | 5.73 |
| `cm_lnr_bench` | 1.51x | 0.551 | 5.23 |
| `s2_frechet` | 1.51x | 0.0128 | 3.97 |
| `sw_skewnormal` | 1.5x | 0.013 | 3.97 |
| `cm_gamma` | 1.5x | 0.0404 | 4.09 |
| `covid19imperial_v2` | 1.5x | 1.39 | 7.63 |
| `Rate_4_model` | 1.46x | 0.00693 | 2.51 |
| `covid19imperial_v3` | 1.45x | 1.39 | 7.64 |
| `Mb_model` | 1.45x | 0.108 | 3.4 |
| `ch13_m13_2` | 1.45x | 0.00946 | 3.13 |
| `gp_regr` | 1.44x | 0.0124 | 5.48 |
| `ch13_m13_3` | 1.43x | 0.00932 | 3.04 |
| `aalto_bern` | 1.41x | 0.00645 | 2.58 |
| `ch13_m13_1` | 1.41x | 0.00885 | 2.96 |
| `s2_gr_student` | 1.4x | 0.0148 | 5.16 |
| `cm_lba2` | 1.39x | 0.0922 | 4.71 |
| `ch16_m16_1` | 1.39x | 0.0511 | 3.04 |
| `ch12_m12_6` | 1.38x | 3.92 | 9.46 |
| `aalto_binom2` | 1.38x | 0.00688 | 2.5 |
| `Rate_1_model` | 1.38x | 0.00627 | 2.38 |
| `Rate_2_model` | 1.37x | 0.00665 | 2.56 |
| `s2_zi_asymlaplace` | 1.37x | 0.0197 | 3.6 |
| `Rate_3_model` | 1.37x | 0.00643 | 2.43 |
| `aalto_binom` | 1.36x | 0.00588 | 2.38 |
| `ch11_m11_9` | 1.36x | 0.00642 | 2.6 |
| `i320_gp_expquad` | 1.35x | 0.0327 | 7.02 |
| `sw_binomial` | 1.35x | 0.0115 | 3.57 |
| `hmm_example` | 1.34x | 0.0594 | 4.15 |
| `aalto_binomb` | 1.34x | 0.00609 | 2.4 |
| `s2_gr_by` | 1.34x | 0.0137 | 4.92 |
| `ctsem_ctsm` | 1.34x | 3.8 | 91.9 |
| `s2_gp_by_gr` | 1.34x | 0.043 | 9.11 |
| `ch14_m14_6x` | 1.32x | 0.179 | 6.88 |
| `ch11_m11_10` | 1.32x | 0.0086 | 3.25 |
| `s2_mm` | 1.32x | 0.015 | 4.68 |
| `s2_me2_nomecor` | 1.3x | 0.0175 | 4.43 |
| `ch11_m_pois` | 1.29x | 0.00652 | 2.52 |
| `sw_gp` | 1.28x | 0.05 | 7.08 |
| `ch12_m12_2` | 1.28x | 0.00954 | 3.96 |
| `ch12_m12_1` | 1.28x | 0.00952 | 3.2 |
| `sw_mono` | 1.28x | 0.0166 | 4.01 |
| `s2_beta_binomial` | 1.28x | 0.0171 | 3.71 |
| `sw_sratio` | 1.27x | 0.141 | 3.77 |
| `sw_mixture` | 1.27x | 0.0251 | 4.82 |
| `ch14_m14_6` | 1.27x | 0.186 | 6.85 |
| `s2_mm_weights` | 1.26x | 0.0153 | 4.65 |
| `nn_rbm1bJ10` | 1.26x | 0.329 | 6 |
| `ch14_m14_9` | 1.24x | 0.705 | 4.93 |
| `cm_exwald` | 1.24x | 0.119 | 4.33 |
| `s2_custom_vint` | 1.24x | 0.0197 | 3.57 |
| `Rate_5_model` | 1.24x | 0.00619 | 2.51 |
| `ch14_m14_1` | 1.24x | 0.0199 | 6.93 |
| `ch14_m14_8nc` | 1.23x | 0.0164 | 5.71 |
| `s2_mo_simo_prior` | 1.23x | 0.0165 | 4 |
| `ch11_m11_8` | 1.22x | 0.00806 | 2.93 |
| `s2_me2` | 1.22x | 0.0199 | 6.7 |
| `ch14_m14_10` | 1.22x | 0.769 | 4.96 |
| `sw_bernoulli` | 1.21x | 0.00865 | 3.28 |
| `i320_mi_nhanes` | 1.2x | 0.0171 | 4.41 |
| `sw_re_gauss` | 1.2x | 0.0135 | 4.57 |
| `s2_cumulative_probit` | 1.19x | 0.0185 | 3.72 |
| `sw_mv_rescor` | 1.19x | 0.0258 | 6.22 |
| `sw_poisson` | 1.18x | 0.00921 | 3.13 |
| `i320_sratio_plain` | 1.18x | 0.146 | 3.96 |
| `ch15_m15_3` | 1.17x | 0.0631 | 2.69 |
| `sw_cratio` | 1.17x | 0.145 | 3.79 |
| `ch14_m14_3` | 1.16x | 0.0564 | 5.95 |
| `ch11_m11_7` | 1.16x | 0.00732 | 2.85 |
| `sw_cratio_cs` | 1.14x | 0.206 | 4.94 |
| `s2_multinomial` | 1.14x | 0.0288 | 4.01 |
| `sw_re_pois` | 1.14x | 0.0133 | 4.49 |
| `s2_categorical_re` | 1.14x | 0.0283 | 5.44 |
| `Survey_model` | 1.14x | 0.127 | 3.08 |
| `ch13_m13_4nc` | 1.13x | 0.0553 | 3.37 |
| `sw_re_bern` | 1.13x | 0.013 | 4.64 |
| `s2_mv_shared_re` | 1.12x | 0.0244 | 7.08 |
| `sw_categorical` | 1.12x | 0.0128 | 4.34 |
| `i320_sratio_cs` | 1.12x | 0.206 | 5.2 |
| `i319_pois_re2` | 1.11x | 0.0356 | 4.84 |
| `ch15_m15_4` | 1.11x | 0.0533 | 2.7 |
| `ch14_m14_8` | 1.1x | 0.0156 | 4.49 |
| `s2_dirichlet` | 1.09x | 0.0354 | 4.4 |
| `sw_re_negbin` | 1.09x | 0.0162 | 4.76 |
| `ch15_m15_7` | 1.09x | 0.0333 | 7.17 |
| `s2_discrete_weibull` | 1.09x | 0.0167 | 3.48 |
| `s2_mmc` | 1.08x | 0.0222 | 6.97 |
| `s2_logistic_normal` | 1.08x | 0.0528 | 6.36 |
| `s2_hurdle_negbin` | 1.08x | 0.0273 | 3.48 |
| `s2_mixture_theta` | 1.07x | 0.0328 | 4.54 |
| `arma11` | 1.07x | 0.0197 | 3 |
| `ch14_m14_7` | 1.06x | 0.0787 | 8.9 |
| `hmm_gaussian` | 1.06x | 0.704 | 5.39 |
| `sw_negbinomial` | 1.06x | 0.0118 | 3.38 |
| `nn_rbm1bJ100` | 1.06x | 876 | 928 |
| `i319_pois_fixed` | 1.06x | 0.0131 | 3.12 |
| `s2_weights_trunc` | 1.06x | 0.023 | 3.61 |
| `sw_re_slope` | 1.05x | 0.018 | 6.78 |
| `i319_negbin_fixed` | 1.05x | 0.0252 | 3.41 |
| `wells_dist100_model` | 1.05x | 0.0408 | 3.25 |
| `sw_trunc` | 1.05x | 0.0223 | 3.58 |
| `wells_dist100ars_model` | 1.04x | 0.0443 | 3.27 |
| `garch11` | 1.04x | 0.0253 | 2.98 |
| `aalto_poisson_simple` | 1.03x | 0.00943 | 2.65 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.03x | 0.0491 | 3.37 |
| `wells_dae_model` | 1.03x | 0.0504 | 3.26 |
| `bones_model` | 1.03x | 0.127 | 3.62 |
| `cm_rdm` | 1.03x | 0.257 | 4.55 |
| `nes_logit_model` | 1.02x | 0.021 | 3.14 |
| `s2_wiener` | 1.01x | 0.0717 | 3.75 |
| `cm_ddm` | 1.01x | 37.2 | 44.2 |
| `wells_daae_c_model` | 1.01x | 0.052 | 3.4 |
| `i320_pois_trunc_ub` | 1.01x | 0.0594 | 3.55 |
| `cm_betadiscrete` | 1.01x | 3.99 | 8.24 |
| `wells_interaction_model` | 1x | 0.0494 | 3.3 |
| `iohmm_reg` | 1x | 0.882 | 6.68 |
| `wells_dae_c_model` | 1x | 0.0477 | 3.45 |
| `wells_dae_inter_model` | 0.99x | 0.0527 | 3.39 |
| `diamonds` | 0.986x | 0.0914 | 3.51 |
| `sw_cumulative` | 0.985x | 0.0524 | 3.82 |
| `i320_pois_trunc_both` | 0.985x | 0.0741 | 3.62 |
| `sw_cumulative_cs` | 0.984x | 0.181 | 4.9 |
| `s2_ar_cov` | 0.98x | 0.0337 | 7.2 |
| `hmm_drive_0` | 0.979x | 0.334 | 5.09 |
| `ch11_m11_5` | 0.973x | 0.0454 | 3.13 |
| `one_comp_mm_elim_abs` | 0.972x | 0.981 | 4.41 |
| `i319_negbin_re` | 0.972x | 0.0392 | 4.78 |
| `s2_cosy` | 0.969x | 0.0319 | 6.5 |
| `hmm_drive_1` | 0.964x | 0.363 | 5.09 |
| `i320_gp_matern32` | 0.96x | 0.0715 | 7 |
| `s2_fcor` | 0.954x | 0.0375 | 4.93 |
| `i319_pois_re` | 0.941x | 0.027 | 4.5 |
| `ch14_m14_11` | 0.922x | 1.57 | 5.38 |
| `ch13_m13_4` | 0.917x | 0.0491 | 3.44 |
| `ch11_m11_4` | 0.899x | 0.0387 | 3.08 |
| `ch14_m14_2` | 0.897x | 0.064 | 7.09 |
| `ch13_m13_5` | 0.896x | 0.0406 | 3.28 |
| `ch13_m13_4b` | 0.891x | 0.0457 | 3.35 |
| `ch13_m13_6` | 0.881x | 0.0492 | 3.43 |
| `ch11_m11_6` | 0.88x | 0.00992 | 3.03 |
| `i319_gauss_re` | 0.865x | 0.0231 | 4.57 |
| `cm_lnr` | 0.852x | 0.317 | 4.66 |
| `aalto_gpareto` | 0.837x | 0.0142 | 2.9 |
| `s2_com_poisson` | 0.257x | 0.501 | 3.76 |
| `cm_invgaussian` | 0.0473x | 2.18 | 4.78 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2775) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5338) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6053) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->

## Fast mode

[Fast mode](fast-mode.md) was measured with the same protocol, binaries and
inputs as the default run above, with the models compiled and run in fast
mode (`corpus_bench.py --fast-math`). Gradient times, 336 models with
paired timings, Apple M3 Ultra, main `84e60796`:

- Fast mode is 2.03x faster than default mode in geometric mean; the median
  model is 1.06x faster. 87 models are at least 2x faster, 43 at least 10x
  and 7 at least 100x. Most of the large gains are models whose likelihood
  repeats over the data, which fast mode evaluates once per distinct row or
  group.
- Against CmdStan, the geometric-mean speedup is 1.95x in default mode and
  3.95x in fast mode; fast mode is faster than CmdStan on 321 of the 336
  models, default mode on 307.
- No model is measurably slower in fast mode: the few that timed up to 6%
  slower were within noise when rechecked in CPU cycles.

The largest gains, gradient time in microseconds:

| model | default | fast | fast vs default |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 40.1 | 0.081 | 493x faster |
| `nes` | 18.2 | 0.072 | 251x faster |
| `logearn_interaction_z` | 8.15 | 0.051 | 159x faster |
| `diamonds` | 33.6 | 0.311 | 108x faster |
| `ch12_m12_4` | 65.8 | 0.666 | 99x faster |
| `aalto_poisson_hurdle` | 7.34 | 0.208 | 35x faster |
| `kidscore_momiq` | 1.39 | 0.053 | 26x faster |
| `M0_model` | 2.45 | 0.241 | 10x faster |

The per-model fast-mode table, raw observations and build identity are in
[`output/corpus-performance-fast/`](../output/corpus-performance-fast/README.md).

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
2,000-gradient estimate as the default comparison. Complete results are sorted
by CmdStan / Stanli gradient ratio, highest first.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
and stock/patched optimized MIR for a scalar normal-likelihood loop are retained
with the [evidence](../output/corpus-performance-vectorized/README.md).

<!--gen:benchmark_vectorized_catalog-->
Run `8ab8978d93458c88`: 342 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 31.5x | 0.159 | 7.54 |
| `gpcm_latent_reg_irt` | 12.5x | 0.332 | 8.43 |
| `grsm_latent_reg_irt` | 11.9x | 0.18 | 7 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 11x | 0.0932 | 3.8 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11x | 0.0896 | 3.63 |
| `radon_county_intercept` | 8.22x | 0.143 | 3.72 |
| `aalto_poisson_hurdle` | 8.17x | 0.0403 | 3.07 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.1x | 0.146 | 3.81 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.05x | 0.144 | 3.82 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 7.84x | 0.148 | 4.01 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 7.76x | 0.147 | 3.97 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.63x | 0.214 | 4.39 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.58x | 0.164 | 3.94 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.54x | 0.212 | 4.22 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.54x | 0.162 | 4.24 |
| `ch12_m12_3_alt` | 7.2x | 0.014 | 2.55 |
| `s2_custom_vreal` | 7.16x | 0.0102 | 3.44 |
| `ch12_m12_3` | 6.84x | 0.0144 | 2.52 |
| `rats_model` | 6.56x | 0.0103 | 3.09 |
| `M0_model` | 6.54x | 0.014 | 2.74 |
| `arK` | 4.82x | 0.0126 | 2.88 |
| `Mt_model` | 4.79x | 0.0174 | 3.5 |
| `Mth_model` | 4.11x | 0.0663 | 4.49 |
| `s2_cumulative_cauchit` | 4.02x | 0.014 | 3.75 |
| `dogs` | 3.48x | 0.0413 | 4.23 |
| `sw_weights` | 3.46x | 0.0109 | 3.46 |
| `GLMM1_model` | 3.38x | 0.0311 | 3.27 |
| `s2_cox_cens` | 3.34x | 0.0148 | 4.98 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.29x | 0.0223 | 4.86 |
| `cm_exgaussian` | 3.24x | 0.0426 | 4.42 |
| `s2_gp_by_approx` | 3.22x | 0.0196 | 6.5 |
| `logmesquite` | 3.12x | 0.01 | 3.42 |
| `logmesquite_logva` | 3.09x | 0.00821 | 3.33 |
| `ch09_m9_1_chains4` | 3.09x | 0.00888 | 3.32 |
| `logmesquite_logvash` | 3.06x | 0.00958 | 3.54 |
| `ch09_m9_1` | 3.04x | 0.00913 | 3.29 |
| `logmesquite_logvas` | 3.04x | 0.00906 | 3.65 |
| `logmesquite_logvolume` | 3.03x | 0.00771 | 3.03 |
| `aalto_grp_aov` | 3.02x | 0.00751 | 3.05 |
| `mesquite` | 2.98x | 0.00929 | 3.38 |
| `radon_county` | 2.96x | 0.0643 | 3.29 |
| `ch15_m15_1` | 2.92x | 0.00845 | 3.41 |
| `s2_hurdle_cumulative` | 2.88x | 0.0185 | 4.17 |
| `s2_cox` | 2.79x | 0.0132 | 4.11 |
| `cm_geg` | 2.78x | 0.0662 | 4.66 |
| `sw_acat` | 2.75x | 0.111 | 4.21 |
| `sw_acat_cs` | 2.7x | 0.159 | 5.43 |
| `Mtbh_model` | 2.7x | 0.0578 | 5 |
| `ch15_m15_5` | 2.69x | 0.00891 | 3.69 |
| `dogs_nonhierarchical` | 2.68x | 0.0587 | 6.9 |
| `eight_schools_centered` | 2.68x | 0.00666 | 2.99 |
| `aalto_grp_prior_mean` | 2.67x | 0.00713 | 3.13 |
| `sw_hurdle_lognormal` | 2.67x | 0.0143 | 3.55 |
| `ch15_m15_2` | 2.65x | 0.00931 | 3.64 |
| `ch09_m5_8s` | 2.62x | 0.00846 | 3.03 |
| `kilpisjarvi` | 2.57x | 0.00831 | 2.8 |
| `ch12_m12_7` | 2.53x | 2.03 | 8.35 |
| `ch12_m12_5` | 2.52x | 2 | 8.37 |
| `s2_shifted_lognormal` | 2.51x | 0.011 | 3.93 |
| `sw_hurdle_pois` | 2.49x | 0.0161 | 3.42 |
| `hierarchical_gp` | 2.48x | 0.0656 | 9.16 |
| `ch09_m5_8s2` | 2.48x | 0.00834 | 3.03 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.46x | 0.00691 | 3.21 |
| `s2_s_cc` | 2.45x | 0.0101 | 4.21 |
| `ldaK5` | 2.44x | 5.21 | 15.3 |
| `s2_gp_approx` | 2.43x | 0.0138 | 5.51 |
| `aalto_grp_prior_mean_var` | 2.43x | 0.00845 | 3.86 |
| `nes` | 2.42x | 0.048 | 3.7 |
| `pilots` | 2.4x | 0.00969 | 3.49 |
| `s2_nlf` | 2.4x | 0.0115 | 4.35 |
| `aalto_lin_std` | 2.37x | 0.0079 | 3.23 |
| `s2_cumulative_cloglog` | 2.36x | 0.0131 | 3.77 |
| `sw_spline_s` | 2.35x | 0.0114 | 4.35 |
| `normal_mixture` | 2.31x | 0.0926 | 3.01 |
| `kidscore_mom_work` | 2.29x | 0.0132 | 3.12 |
| `Mh_model` | 2.28x | 0.0463 | 3.36 |
| `dugongs_model` | 2.28x | 0.0087 | 3.03 |
| `kidscore_interaction_c2` | 2.27x | 0.0131 | 3.15 |
| `kidscore_interaction_z` | 2.27x | 0.0134 | 3.27 |
| `aalto_lin` | 2.27x | 0.00773 | 3.05 |
| `sw_vonmises` | 2.27x | 0.0105 | 3.65 |
| `kidscore_interaction` | 2.26x | 0.0131 | 3.21 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.24x | 0.101 | 3.32 |
| `extra_hurdle_poisson` | 2.23x | 0.00708 | 2.59 |
| `ch15_m15_6` | 2.23x | 0.00793 | 3.19 |
| `losscurve_sislob` | 2.23x | 0.0163 | 4.71 |
| `logearn_height_male` | 2.22x | 0.0183 | 3.06 |
| `s2_sar` | 2.22x | 0.0184 | 4.24 |
| `sw_nonlinear` | 2.21x | 0.0102 | 3.89 |
| `s2_sar_error` | 2.21x | 0.018 | 4.33 |
| `logearn_logheight_male` | 2.21x | 0.0182 | 3.09 |
| `ch09_m9_4` | 2.2x | 0.00699 | 2.58 |
| `logearn_interaction_z` | 2.2x | 0.0237 | 3.32 |
| `low_dim_gauss_mix` | 2.19x | 0.107 | 3.43 |
| `logearn_interaction` | 2.19x | 0.0229 | 3.18 |
| `ch14_m14_5` | 2.18x | 0.0121 | 3.04 |
| `dogs_hierarchical` | 2.17x | 0.0553 | 2.86 |
| `ch09_m9_5` | 2.16x | 0.00658 | 2.56 |
| `sw_lognormal` | 2.16x | 0.00976 | 3.63 |
| `kidscore_interaction_c` | 2.16x | 0.0127 | 3.18 |
| `kidscore_momhsiq` | 2.14x | 0.0115 | 3.1 |
| `s2_mi_lognormal` | 2.11x | 0.014 | 4.56 |
| `ch09_m9_3` | 2.11x | 0.00619 | 2.52 |
| `GLMM_Poisson_model` | 2.1x | 0.00957 | 3.85 |
| `ch09_m9_2` | 2.1x | 0.00667 | 2.54 |
| `sesame_one_pred_a` | 2.1x | 0.00921 | 2.83 |
| `logearn_height` | 2.08x | 0.0142 | 2.9 |
| `s2_s_by` | 2.08x | 0.0226 | 5.74 |
| `s2_nl_noloop` | 2.07x | 0.00991 | 3.83 |
| `ch14_m14_4x` | 2.07x | 0.0105 | 2.87 |
| `GLM_Poisson_model` | 2.06x | 0.00835 | 3.3 |
| `seeds_centered_model` | 2.04x | 0.00939 | 3.91 |
| `sw_se` | 2.03x | 0.00947 | 3.44 |
| `kidscore_momhs` | 2.03x | 0.0105 | 2.92 |
| `radon_pooled` | 2.03x | 0.0929 | 3.02 |
| `ldaK2` | 2.03x | 0.12 | 3.88 |
| `sw_student` | 2.02x | 0.0101 | 3.8 |
| `sw_zi_poisson` | 2.02x | 0.0148 | 3.39 |
| `s2_car_icar` | 2x | 0.013 | 4.38 |
| `sw_dist_sigma` | 2x | 0.0111 | 3.79 |
| `earn_height` | 2x | 0.0142 | 2.86 |
| `aalto_lin_std_t` | 1.99x | 0.00842 | 3.46 |
| `log10earn_height` | 1.99x | 0.0151 | 2.89 |
| `ch14_m14_4` | 1.98x | 0.0107 | 2.87 |
| `election88_full` | 1.98x | 0.499 | 6.04 |
| `kidscore_momiq` | 1.93x | 0.0102 | 2.91 |
| `ch16_m16_4` | 1.92x | 0.0102 | 3.35 |
| `normal_mixture_k` | 1.91x | 0.442 | 4.2 |
| `sw_spline_t2` | 1.91x | 0.014 | 4.82 |
| `multi_occupancy` | 1.9x | 0.0833 | 6.56 |
| `s2_t2_by` | 1.9x | 0.0208 | 5.68 |
| `s2_cens_interval` | 1.89x | 0.0131 | 3.74 |
| `ch13_m13_7` | 1.87x | 0.00616 | 2.33 |
| `s2_gev` | 1.87x | 0.0158 | 3.84 |
| `sw_me` | 1.83x | 0.0126 | 4.27 |
| `ch13_m13_7nc` | 1.83x | 0.00614 | 2.34 |
| `s2_car` | 1.83x | 0.0161 | 4.7 |
| `ch09_mp` | 1.81x | 0.00587 | 2.32 |
| `ch11_m11_11` | 1.81x | 0.00925 | 3.55 |
| `accel_gp` | 1.8x | 0.0304 | 6.62 |
| `soil_incubation` | 1.79x | 0.0762 | 4.03 |
| `s2_car_esicar` | 1.77x | 0.0153 | 4.79 |
| `sw_asymlaplace` | 1.77x | 0.0151 | 3.64 |
| `lotka_volterra` | 1.76x | 0.0578 | 4.55 |
| `sw_zi_binomial` | 1.74x | 0.0166 | 3.43 |
| `sw_hurdle_gamma` | 1.73x | 0.0169 | 3.54 |
| `sw_mi` | 1.72x | 0.0127 | 3.83 |
| `2pl_latent_reg_irt` | 1.67x | 0.147 | 5.76 |
| `bym2_offset_only` | 1.66x | 0.0889 | 4.5 |
| `sw_ar` | 1.66x | 0.0131 | 4.25 |
| `ch15_m15_9` | 1.65x | 0.0172 | 3.15 |
| `s2_threading` | 1.65x | 0.0102 | 3.38 |
| `hier_2pl` | 1.65x | 0.411 | 7.41 |
| `cm_loggamma` | 1.64x | 0.0447 | 4.65 |
| `cm_weibull` | 1.64x | 0.0375 | 4.06 |
| `accel_splines` | 1.63x | 0.025 | 4.59 |
| `sw_zi_negbin` | 1.62x | 0.0194 | 3.48 |
| `sw_ma` | 1.6x | 0.0143 | 4.23 |
| `cm_betagate` | 1.59x | 0.0859 | 4.32 |
| `cm_lba1` | 1.59x | 0.0604 | 4.97 |
| `cm_logstudent` | 1.59x | 0.0391 | 4.22 |
| `s2_mv_subset` | 1.58x | 0.0117 | 3.49 |
| `ch15_m15_8` | 1.58x | 0.0159 | 2.9 |
| `cm_invweibull` | 1.58x | 0.039 | 4.07 |
| `seeds_stanified_model` | 1.57x | 0.0104 | 3.65 |
| `seeds_model` | 1.57x | 0.0103 | 3.73 |
| `cm_choco` | 1.57x | 0.113 | 5.15 |
| `cm_lognormal` | 1.57x | 0.0754 | 5.52 |
| `cm_lnr_bench` | 1.56x | 0.543 | 8.96 |
| `cm_bisa` | 1.56x | 0.0377 | 4.16 |
| `cm_logweibull` | 1.55x | 0.0373 | 4.09 |
| `covid19imperial_v3` | 1.54x | 1.38 | 7.6 |
| `surgical_model` | 1.53x | 0.00877 | 3.43 |
| `prophet` | 1.52x | 0.0931 | 5.68 |
| `cm_gamma` | 1.52x | 0.0381 | 4.11 |
| `sw_gaussian` | 1.52x | 0.00932 | 3.31 |
| `s2_index_mi` | 1.52x | 0.015 | 4.17 |
| `ctsem_ctsm` | 1.51x | 3.71 | 441 |
| `s2_dist_sigma_re` | 1.49x | 0.0146 | 4.96 |
| `cm_invgamma` | 1.49x | 0.0401 | 4.09 |
| `sw_cratio_cs` | 1.48x | 0.205 | 5.3 |
| `s2_unstr` | 1.48x | 0.0376 | 8.7 |
| `Mb_model` | 1.48x | 0.112 | 3.39 |
| `i320_sratio_cs` | 1.47x | 0.202 | 5.58 |
| `sw_gamma` | 1.47x | 0.0105 | 3.76 |
| `sw_arma` | 1.47x | 0.0161 | 4.34 |
| `ch13_m13_1` | 1.47x | 0.0093 | 2.98 |
| `lsat_model` | 1.46x | 0.0974 | 3.86 |
| `covid19imperial_v2` | 1.45x | 1.39 | 7.57 |
| `gp_pois_regr` | 1.45x | 0.0109 | 5.64 |
| `ch13_m13_3` | 1.45x | 0.00933 | 3.01 |
| `ch13_m13_2` | 1.45x | 0.00933 | 3.12 |
| `gp_regr` | 1.45x | 0.0113 | 5.46 |
| `aalto_binom` | 1.44x | 0.00575 | 2.37 |
| `sw_mv_norescor` | 1.44x | 0.0128 | 3.49 |
| `s2_rate` | 1.42x | 0.00935 | 3.6 |
| `aalto_bern` | 1.42x | 0.00655 | 2.56 |
| `s2_gr_student` | 1.41x | 0.0145 | 5.29 |
| `sw_skewnormal` | 1.41x | 0.0137 | 3.92 |
| `s2_zoi_beta` | 1.4x | 0.0234 | 3.77 |
| `Rate_1_model` | 1.4x | 0.00573 | 2.37 |
| `sw_cumulative_cs` | 1.39x | 0.183 | 5.17 |
| `Rate_4_model` | 1.39x | 0.00629 | 2.51 |
| `s2_mm_weights` | 1.38x | 0.0154 | 4.7 |
| `hmm_example` | 1.38x | 0.0593 | 4.14 |
| `aalto_binom2` | 1.37x | 0.00633 | 2.5 |
| `s2_zi_beta` | 1.37x | 0.0208 | 3.72 |
| `ch11_m_pois` | 1.37x | 0.00614 | 2.52 |
| `Rate_2_model` | 1.36x | 0.0067 | 2.55 |
| `GLM_Binomial_model` | 1.36x | 0.00934 | 3.28 |
| `ch16_m16_1` | 1.36x | 0.0508 | 3.04 |
| `sw_exgaussian` | 1.35x | 0.0108 | 3.87 |
| `cm_lba2` | 1.35x | 0.0884 | 7.45 |
| `irt_2pl` | 1.35x | 0.0422 | 4.27 |
| `s2_cumulative_probit` | 1.35x | 0.0183 | 3.69 |
| `blr` | 1.34x | 0.00759 | 3.06 |
| `aalto_binomb` | 1.33x | 0.00591 | 2.42 |
| `ch11_m11_9` | 1.33x | 0.00726 | 2.6 |
| `wells_dist` | 1.31x | 0.0565 | 3.07 |
| `s2_gr_by` | 1.31x | 0.0145 | 4.93 |
| `ch12_m12_6` | 1.3x | 4.01 | 9.31 |
| `ch12_m12_2` | 1.3x | 0.01 | 3.97 |
| `s2_zi_asymlaplace` | 1.29x | 0.02 | 3.67 |
| `Rate_3_model` | 1.28x | 0.00664 | 2.4 |
| `Rate_5_model` | 1.28x | 0.00676 | 2.49 |
| `sw_beta` | 1.26x | 0.0127 | 3.9 |
| `sw_mixture` | 1.25x | 0.024 | 4.74 |
| `cm_exwald` | 1.25x | 0.113 | 5.98 |
| `nn_rbm1bJ10` | 1.25x | 0.331 | 5.85 |
| `sw_re_gauss` | 1.25x | 0.0138 | 4.59 |
| `ch12_m12_1` | 1.25x | 0.00947 | 3.19 |
| `i320_gp_expquad` | 1.24x | 0.0352 | 7.31 |
| `ch11_m11_8` | 1.24x | 0.00765 | 2.92 |
| `sw_gp` | 1.23x | 0.0508 | 7.32 |
| `ch14_m14_6` | 1.23x | 0.185 | 6.96 |
| `i320_mi_nhanes` | 1.23x | 0.0172 | 4.43 |
| `sw_mv_rescor` | 1.23x | 0.0259 | 6.11 |
| `ch14_m14_6x` | 1.23x | 0.19 | 6.93 |
| `s2_mm` | 1.23x | 0.0151 | 4.71 |
| `ch14_m14_1` | 1.22x | 0.0198 | 6.9 |
| `ch14_m14_9` | 1.21x | 0.692 | 5.07 |
| `sw_categorical` | 1.2x | 0.0124 | 4.37 |
| `ch14_m14_8nc` | 1.19x | 0.0165 | 5.99 |
| `ch14_m14_10` | 1.19x | 0.769 | 5.15 |
| `ch11_m11_7` | 1.19x | 0.00723 | 2.85 |
| `s2_custom_vint` | 1.19x | 0.0196 | 3.54 |
| `sw_bernoulli` | 1.19x | 0.00919 | 3.27 |
| `s2_categorical_re` | 1.18x | 0.0296 | 5.45 |
| `s2_frechet` | 1.17x | 0.0135 | 3.95 |
| `sw_re_bern` | 1.17x | 0.0138 | 4.66 |
| `sw_weibull` | 1.17x | 0.013 | 3.92 |
| `s2_me2` | 1.16x | 0.0209 | 6.68 |
| `ch14_m14_3` | 1.16x | 0.0549 | 5.92 |
| `logistic_regression_rhs` | 1.15x | 0.108 | 5.14 |
| `sw_binomial` | 1.14x | 0.0113 | 3.49 |
| `sw_re_negbin` | 1.14x | 0.0158 | 4.76 |
| `ch14_m14_8` | 1.14x | 0.0155 | 4.77 |
| `ch11_m11_10` | 1.14x | 0.008 | 3.23 |
| `sw_poisson` | 1.14x | 0.00893 | 3.11 |
| `sw_re_pois` | 1.14x | 0.0135 | 4.51 |
| `Survey_model` | 1.13x | 0.128 | 3.09 |
| `s2_me2_nomecor` | 1.12x | 0.019 | 4.53 |
| `s2_multinomial` | 1.11x | 0.0299 | 3.95 |
| `sw_negbinomial` | 1.11x | 0.0109 | 3.41 |
| `sw_re_slope` | 1.11x | 0.0194 | 6.83 |
| `sw_trunc` | 1.09x | 0.0217 | 3.54 |
| `sw_cratio` | 1.09x | 0.142 | 4.04 |
| `s2_beta_binomial` | 1.08x | 0.0183 | 3.7 |
| `sw_sratio` | 1.08x | 0.142 | 4.05 |
| `nn_rbm1bJ100` | 1.08x | 866 | 933 |
| `s2_mv_shared_re` | 1.07x | 0.0247 | 7.17 |
| `i320_sratio_plain` | 1.06x | 0.145 | 4.18 |
| `s2_weights_trunc` | 1.05x | 0.0229 | 3.59 |
| `s2_hurdle_negbin` | 1.05x | 0.0267 | 3.51 |
| `ch15_m15_7` | 1.05x | 0.0327 | 7.62 |
| `i319_negbin_fixed` | 1.04x | 0.0255 | 3.41 |
| `hmm_gaussian` | 1.04x | 0.704 | 5.39 |
| `wells_dist100_model` | 1.04x | 0.0438 | 3.23 |
| `s2_discrete_weibull` | 1.04x | 0.0167 | 3.51 |
| `aalto_poisson_simple` | 1.04x | 0.00959 | 2.67 |
| `wells_dae_model` | 1.03x | 0.0486 | 3.28 |
| `s2_mmc` | 1.03x | 0.0226 | 7.02 |
| `one_comp_mm_elim_abs` | 1.03x | 0.98 | 4.8 |
| `iohmm_reg` | 1.03x | 0.871 | 6.82 |
| `ch14_m14_7` | 1.03x | 0.0806 | 8.86 |
| `i319_pois_re2` | 1.02x | 0.0371 | 4.82 |
| `wells_daae_c_model` | 1.02x | 0.0499 | 3.42 |
| `wells_dae_c_model` | 1.02x | 0.0475 | 3.41 |
| `wells_dist100ars_model` | 1.02x | 0.0457 | 3.27 |
| `bones_model` | 1.02x | 0.127 | 3.6 |
| `i319_pois_fixed` | 1.02x | 0.0138 | 3.13 |
| `sw_mono` | 1.02x | 0.0155 | 4.13 |
| `hmm_drive_1` | 1.01x | 0.356 | 5.08 |
| `i320_pois_trunc_ub` | 1.01x | 0.0592 | 3.5 |
| `diamonds` | 1.01x | 0.0913 | 3.49 |
| `s2_dirichlet` | 1x | 0.0368 | 4.39 |
| `s2_mo_simo_prior` | 1x | 0.0164 | 4.1 |
| `wells_dae_inter_model` | 1x | 0.0521 | 3.4 |
| `cm_rdm` | 1x | 0.236 | 10.8 |
| `s2_ar_cov` | 0.995x | 0.0333 | 7.61 |
| `wells_interaction_model` | 0.994x | 0.0493 | 3.3 |
| `s2_mixture_theta` | 0.991x | 0.0329 | 4.47 |
| `s2_logistic_normal` | 0.987x | 0.0538 | 6.73 |
| `cm_betadiscrete` | 0.986x | 3.91 | 8.12 |
| `i319_negbin_re` | 0.982x | 0.0383 | 4.79 |
| `s2_wiener` | 0.979x | 0.0723 | 3.75 |
| `nes_logit_model` | 0.974x | 0.021 | 3.18 |
| `cm_ddm` | 0.974x | 36.8 | 44.9 |
| `i320_pois_trunc_both` | 0.973x | 0.076 | 3.58 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.964x | 0.0501 | 3.36 |
| `ch11_m11_5` | 0.963x | 0.0452 | 3.13 |
| `s2_fcor` | 0.963x | 0.0376 | 4.89 |
| `s2_cosy` | 0.959x | 0.0322 | 6.72 |
| `hmm_drive_0` | 0.955x | 0.351 | 5.1 |
| `sw_cumulative` | 0.949x | 0.0552 | 3.78 |
| `ch13_m13_6` | 0.941x | 0.0483 | 3.41 |
| `ch13_m13_4b` | 0.935x | 0.0429 | 3.34 |
| `ch13_m13_4nc` | 0.93x | 0.0521 | 3.37 |
| `i319_pois_re` | 0.923x | 0.0264 | 4.52 |
| `i319_gauss_re` | 0.923x | 0.023 | 4.58 |
| `ch13_m13_5` | 0.913x | 0.0392 | 3.28 |
| `ch11_m11_6` | 0.913x | 0.00941 | 3 |
| `i320_gp_matern32` | 0.912x | 0.0737 | 7.23 |
| `ch14_m14_11` | 0.899x | 1.6 | 5.82 |
| `cm_lnr` | 0.899x | 0.317 | 14 |
| `ch11_m11_4` | 0.893x | 0.0393 | 3.05 |
| `ch14_m14_2` | 0.878x | 0.0666 | 7.11 |
| `ch13_m13_4` | 0.869x | 0.0497 | 3.43 |
| `ch15_m15_3` | 0.865x | 0.0605 | 2.7 |
| `garch11` | 0.841x | 0.0258 | 2.95 |
| `ch15_m15_4` | 0.825x | 0.0517 | 2.76 |
| `aalto_gpareto` | 0.792x | 0.0141 | 3.06 |
| `arma11` | 0.707x | 0.019 | 2.98 |
| `s2_com_poisson` | 0.206x | 0.506 | 4.76 |
| `cm_invgaussian` | 0.0513x | 2.15 | 33.8 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2775) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5322) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6037) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->
