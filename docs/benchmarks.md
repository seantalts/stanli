# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling, it reuses a prepared autodiff graph and batches
independent work. This can shorten the first fit and repeated gradient
evaluations; the gain depends on the model.

## <!--gen:benchmark_corpus_headline-->2.63× estimated corpus speedup<!--/gen-->

<!--gen:benchmark_corpus_time-->Estimated corpus time: Stanli 985.3 s vs CmdStan 2,595.0 s (2.63x CmdStan / Stanli; 337/343 models). Setup + 2,000 gradients per model. Full-corpus total unavailable: 6 models have incomplete measurements.<!--/gen-->

These measurements use the [frozen October 13 runtime snapshot](../output/corpus-performance/source-provenance.json),
including its retained local changes.

The current comparison covers **<!--gen:benchmark_models-->343<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->337<!--/gen--> accepted gradient
comparisons, the median paired speedup is **<!--gen:corpus_median-->2.17x<!--/gen-->**.
The tables cover every model, with incomplete results listed separately.

The headline answers how much time the fixed corpus workload takes by dividing
the engines' summed seconds. Expensive models contribute more to that total.
The median gradient speedup describes the typical model's repeated evaluation
cost and gives each model equal weight. For a normalized suite score, a geometric
mean of per-model ratios also gives each model equal weight; [SPEC uses that
convention](https://www.spec.org/cpu2017/Docs/runrules.html).
An arithmetic mean of speedup ratios can be dominated by extreme gains and
changes meaning when the comparison direction is reversed, so we do not use it
as the headline. All these summaries accompany the per-model results and coverage.

<a id="benchmark-method"></a>

## How we measure

**Gradient speedup is CmdStan time / Stanli time:** above 1x favors Stanli.
Both engines evaluate the same log density and full gradient at a shared
unconstrained point. After warmup, six alternating pairs give a median
speedup. Every accepted pair must pass its numerical comparison.

**Corpus time is the sum of setup + 2,000 × median warm gradient time for every model.**
The headline ratio divides summed CmdStan seconds by summed Stanli seconds.
When any model lacks an accepted gradient comparison or complete setup timings,
the headline shows the subtotal for the same completed models in both engines
and marks the full-corpus total unavailable.
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
Run `694ea75f15c86c92` (2026-10-13): 343 models, 6 alternating gradient pairs.

Estimated corpus time: Stanli 985.3 s vs CmdStan 2,595.0 s (2.63x CmdStan / Stanli; 337/343 models). Setup + 2,000 gradients per model. Full-corpus total unavailable: 6 models have incomplete measurements.

| Model | CmdStan / Stanli gradient | Stanli estimate (s) | CmdStan estimate (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 41.2x | 0.155 | 8.78 |
| `gpcm_latent_reg_irt` | 15.4x | 0.349 | 8.88 |
| `grsm_latent_reg_irt` | 13.9x | 0.179 | 7.05 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.1x | 0.0901 | 3.63 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 0.0913 | 3.83 |
| `radon_county_intercept` | 10.1x | 0.144 | 3.92 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 10.1x | 0.149 | 4.19 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 10.1x | 0.146 | 3.97 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 10x | 0.145 | 4.02 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 9.86x | 0.15 | 4.19 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 9.58x | 0.216 | 4.56 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 9.32x | 0.22 | 4.65 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 9.25x | 0.167 | 4.41 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 9.12x | 0.166 | 4.14 |
| `dogs` | 8.78x | 0.0446 | 3.8 |
| `radon_pooled` | 8.37x | 0.0932 | 3.4 |
| `aalto_poisson_hurdle` | 8.16x | 0.039 | 3.07 |
| `s2_custom_vreal` | 7.74x | 0.0103 | 3.49 |
| `rats_model` | 7.61x | 0.00986 | 3.13 |
| `arK` | 7.52x | 0.0121 | 2.91 |
| `ch12_m12_3_alt` | 7.22x | 0.014 | 2.53 |
| `ch16_m16_1` | 6.58x | 0.0201 | 3.06 |
| `Mt_model` | 6.56x | 0.0164 | 3.49 |
| `ch12_m12_3` | 6.52x | 0.0146 | 2.53 |
| `M0_model` | 6.19x | 0.0146 | 2.73 |
| `sw_nonlinear` | 5.27x | 0.0112 | 3.89 |
| `ch09_m5_8s` | 4.93x | 0.00852 | 2.87 |
| `ch09_m5_8s2` | 4.74x | 0.00794 | 2.89 |
| `s2_cumulative_cauchit` | 4.59x | 0.0139 | 3.7 |
| `ch14_m14_5` | 4.51x | 0.0117 | 2.88 |
| `s2_nl_noloop` | 4.3x | 0.0102 | 3.95 |
| `mesquite` | 4.29x | 0.00838 | 3.12 |
| `logmesquite` | 4.25x | 0.00933 | 3.17 |
| `ch15_m15_1` | 4.23x | 0.00884 | 3.2 |
| `logmesquite_logvash` | 4.22x | 0.00953 | 3.33 |
| `logmesquite_logvas` | 4.21x | 0.00938 | 3.43 |
| `s2_car_icar` | 4.17x | 0.0115 | 4.44 |
| `aalto_grp_aov` | 4.16x | 0.00666 | 3.08 |
| `Mth_model` | 4.13x | 0.0682 | 4.53 |
| `logmesquite_logva` | 4.11x | 0.00877 | 3.19 |
| `ch15_m15_2` | 3.92x | 0.00982 | 3.43 |
| `election88_full` | 3.9x | 0.522 | 6.1 |
| `nes` | 3.89x | 0.049 | 3.37 |
| `ch14_m14_4x` | 3.84x | 0.0105 | 2.82 |
| `sw_me` | 3.83x | 0.0119 | 4.17 |
| `s2_s_cc` | 3.82x | 0.0107 | 4.31 |
| `ch14_m14_4` | 3.78x | 0.0108 | 2.8 |
| `kilpisjarvi` | 3.77x | 0.0079 | 2.82 |
| `s2_me2_nomecor` | 3.73x | 0.0141 | 4.43 |
| `sw_dist_sigma` | 3.73x | 0.0122 | 3.86 |
| `s2_dist_sigma_re` | 3.72x | 0.0133 | 4.98 |
| `kidscore_interaction_c` | 3.7x | 0.0124 | 3.09 |
| `s2_nlf` | 3.7x | 0.0117 | 3.97 |
| `sw_spline_s` | 3.7x | 0.0114 | 4.49 |
| `kidscore_interaction_c2` | 3.69x | 0.0132 | 3.03 |
| `ch09_m9_1` | 3.69x | 0.00889 | 3.1 |
| `sw_weights` | 3.68x | 0.0103 | 3.54 |
| `ch09_m9_1_chains4` | 3.66x | 0.00911 | 3.08 |
| `logmesquite_logvolume` | 3.64x | 0.00795 | 3.02 |
| `s2_mm` | 3.6x | 0.0143 | 4.7 |
| <code>eight_schools_</code><br><code>noncentered</code> | 3.58x | 0.00724 | 3.3 |
| `kidscore_mom_work` | 3.57x | 0.0133 | 2.97 |
| `s2_cox_cens` | 3.57x | 0.0157 | 5.14 |
| `s2_gp_approx` | 3.54x | 0.0137 | 5.26 |
| `s2_mm_weights` | 3.54x | 0.0129 | 4.69 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.51x | 0.0231 | 4.96 |
| `sw_se` | 3.5x | 0.00931 | 3.53 |
| `kidscore_interaction` | 3.49x | 0.013 | 3.09 |
| `GLMM1_model` | 3.48x | 0.0311 | 3.31 |
| `s2_hurdle_cumulative` | 3.46x | 0.0184 | 4.2 |
| `kidscore_interaction_z` | 3.45x | 0.0131 | 3.12 |
| `s2_shifted_lognormal` | 3.44x | 0.0106 | 3.93 |
| `aalto_grp_prior_mean` | 3.43x | 0.00753 | 3.19 |
| `ch12_m12_7` | 3.41x | 1.95 | 9.88 |
| `s2_gp_by_approx` | 3.39x | 0.0196 | 6.05 |
| `i319_pois_re2` | 3.39x | 0.0206 | 4.88 |
| `sw_spline_t2` | 3.38x | 0.0149 | 4.96 |
| `logearn_interaction` | 3.37x | 0.0234 | 3.07 |
| `s2_gr_by` | 3.36x | 0.0128 | 4.99 |
| `ch15_m15_5` | 3.36x | 0.00915 | 3.26 |
| `logearn_height_male` | 3.36x | 0.0187 | 2.98 |
| `ch15_m15_8` | 3.35x | 0.013 | 2.9 |
| `ch15_m15_9` | 3.31x | 0.0142 | 3.04 |
| `radon_county` | 3.3x | 0.0639 | 3.31 |
| `logearn_interaction_z` | 3.29x | 0.0235 | 3.19 |
| `ch16_m16_4` | 3.29x | 0.0108 | 2.94 |
| `s2_s_by` | 3.29x | 0.0219 | 5.92 |
| `ch12_m12_5` | 3.28x | 2.03 | 9.95 |
| `logearn_logheight_male` | 3.28x | 0.0189 | 3.01 |
| `sw_lognormal` | 3.27x | 0.00949 | 3.71 |
| `s2_car` | 3.25x | 0.014 | 4.49 |
| `s2_t2_by` | 3.24x | 0.021 | 5.88 |
| `aalto_grp_prior_mean_var` | 3.2x | 0.00848 | 3.92 |
| `sw_acat` | 3.17x | 0.118 | 4.22 |
| `kidscore_momhsiq` | 3.17x | 0.0117 | 2.97 |
| `i319_gauss_re` | 3.15x | 0.0156 | 4.62 |
| `s2_car_esicar` | 3.15x | 0.0152 | 4.5 |
| `sw_re_gauss` | 3.14x | 0.0118 | 4.62 |
| `eight_schools_centered` | 3.1x | 0.00687 | 3 |
| `aalto_lin` | 3.09x | 0.00822 | 3.1 |
| `Mtbh_model` | 3.08x | 0.0568 | 5.12 |
| `pilots` | 3.03x | 0.00908 | 3.4 |
| `ch15_m15_6` | 3.01x | 0.00843 | 3.01 |
| `sw_student` | 3.01x | 0.011 | 3.86 |
| `s2_gr_student` | 2.99x | 0.0132 | 5.2 |
| `s2_cox` | 2.98x | 0.0126 | 4.18 |
| `s2_mi_lognormal` | 2.96x | 0.0131 | 4.63 |
| `sw_re_pois` | 2.9x | 0.0121 | 4.51 |
| `logearn_height` | 2.9x | 0.0145 | 2.89 |
| `aalto_lin_std` | 2.87x | 0.00831 | 3.31 |
| `blr` | 2.86x | 0.00766 | 3.12 |
| `log10earn_height` | 2.86x | 0.0148 | 2.85 |
| `sesame_one_pred_a` | 2.85x | 0.00847 | 2.84 |
| `sw_hurdle_lognormal` | 2.85x | 0.0155 | 3.59 |
| `i319_pois_re` | 2.83x | 0.0164 | 4.54 |
| `s2_cumulative_cloglog` | 2.83x | 0.0131 | 3.69 |
| `cm_geg` | 2.8x | 0.062 | 4.03 |
| `accel_gp` | 2.78x | 0.0254 | 5.48 |
| `GLM_Poisson_model` | 2.77x | 0.00838 | 3.31 |
| `GLMM_Poisson_model` | 2.76x | 0.00926 | 3.84 |
| `kidscore_momiq` | 2.76x | 0.01 | 2.87 |
| `kidscore_momhs` | 2.75x | 0.0103 | 2.88 |
| `sw_acat_cs` | 2.74x | 0.159 | 5.41 |
| `earn_height` | 2.74x | 0.0142 | 2.84 |
| `bym2_offset_only` | 2.73x | 0.0878 | 4.45 |
| `dogs_nonhierarchical` | 2.71x | 0.0584 | 7.05 |
| `accel_splines` | 2.66x | 0.0209 | 4.46 |
| `losscurve_sislob` | 2.65x | 0.0165 | 4.34 |
| `ch13_m13_4nc` | 2.65x | 0.03 | 3.37 |
| `sw_gamma` | 2.63x | 0.0103 | 3.78 |
| `ldaK5` | 2.6x | 5.34 | 16.3 |
| `cm_exgaussian` | 2.57x | 0.0432 | 3.74 |
| `s2_sar` | 2.57x | 0.0182 | 4.1 |
| `sw_hurdle_pois` | 2.55x | 0.0154 | 3.45 |
| `ch11_m11_10` | 2.54x | 0.0076 | 3.25 |
| `s2_mmc` | 2.54x | 0.0177 | 7.07 |
| `sw_re_bern` | 2.52x | 0.0117 | 4.68 |
| `sw_skewnormal` | 2.5x | 0.0129 | 3.99 |
| `hierarchical_gp` | 2.48x | 0.0646 | 9.24 |
| `ldaK2` | 2.43x | 0.12 | 3.98 |
| `ch15_m15_3` | 2.42x | 0.035 | 2.71 |
| `ch09_m9_2` | 2.4x | 0.00659 | 2.53 |
| `ch14_m14_3` | 2.4x | 0.0352 | 5.96 |
| `s2_sar_error` | 2.4x | 0.0185 | 4.18 |
| `s2_rate` | 2.39x | 0.00945 | 3.63 |
| `logistic_regression_rhs` | 2.38x | 0.105 | 5.13 |
| `Mh_model` | 2.38x | 0.0473 | 3.39 |
| `ch09_m9_3` | 2.37x | 0.0067 | 2.52 |
| `dugongs_model` | 2.34x | 0.00819 | 3.03 |
| `aalto_lin_std_t` | 2.34x | 0.0091 | 3.52 |
| `i320_mi_nhanes` | 2.33x | 0.0171 | 4.55 |
| `seeds_centered_model` | 2.31x | 0.00986 | 3.88 |
| `ch09_m9_5` | 2.31x | 0.00668 | 2.57 |
| `s2_me2` | 2.3x | 0.0166 | 6.78 |
| `extra_hurdle_poisson` | 2.3x | 0.00725 | 2.61 |
| `s2_gev` | 2.29x | 0.0165 | 3.81 |
| `ch11_m11_5` | 2.28x | 0.0276 | 3.13 |
| `lsat_model` | 2.27x | 0.0955 | 3.98 |
| `ch15_m15_4` | 2.27x | 0.0308 | 2.71 |
| `ch13_m13_4b` | 2.23x | 0.024 | 3.34 |
| `s2_mv_shared_re` | 2.22x | 0.0211 | 7.17 |
| `sw_vonmises` | 2.21x | 0.01 | 3.73 |
| `low_dim_gauss_mix` | 2.21x | 0.103 | 3.45 |
| `normal_mixture` | 2.2x | 0.0941 | 2.78 |
| `soil_incubation` | 2.19x | 0.0754 | 3.62 |
| `ch09_m9_4` | 2.19x | 0.00678 | 2.57 |
| `ch13_m13_4` | 2.18x | 0.0283 | 3.46 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.18x | 0.101 | 3.34 |
| `ch13_m13_6` | 2.17x | 0.0282 | 3.43 |
| `2pl_latent_reg_irt` | 2.16x | 0.404 | 6.23 |
| `dogs_hierarchical` | 2.16x | 0.0559 | 2.86 |
| `multi_occupancy` | 2.12x | 0.0824 | 6.23 |
| `irt_2pl` | 2.12x | 0.0431 | 4.32 |
| `lotka_volterra` | 2.11x | 0.0556 | 4.49 |
| `ch13_m13_7` | 2.09x | 0.00576 | 2.32 |
| `sw_zi_poisson` | 2.09x | 0.0154 | 3.39 |
| `hier_2pl` | 2.08x | 0.416 | 7.96 |
| `ch11_m11_6` | 2.06x | 0.00822 | 3.02 |
| `seeds_model` | 2.06x | 0.0096 | 3.71 |
| `seeds_stanified_model` | 2.03x | 0.00926 | 3.59 |
| `ch11_m11_8` | 2.02x | 0.0072 | 2.94 |
| `ch11_m11_4` | 2.01x | 0.0247 | 3.04 |
| `s2_index_mi` | 2x | 0.0143 | 4.26 |
| `prophet` | 2x | 0.0928 | 5.13 |
| `ch13_m13_5` | 1.99x | 0.0248 | 3.3 |
| `sw_re_slope` | 1.99x | 0.0167 | 6.85 |
| `normal_mixture_k` | 1.97x | 0.44 | 4.24 |
| `ch09_mp` | 1.96x | 0.00616 | 2.32 |
| `sw_poisson` | 1.96x | 0.00922 | 3.12 |
| `s2_threading` | 1.96x | 0.00977 | 3.39 |
| `i319_pois_fixed` | 1.95x | 0.0107 | 3.13 |
| `ch13_m13_7nc` | 1.94x | 0.00618 | 2.35 |
| `sw_exgaussian` | 1.93x | 0.0108 | 3.92 |
| `s2_cens_interval` | 1.89x | 0.0137 | 3.8 |
| `sw_hurdle_gamma` | 1.87x | 0.0171 | 3.58 |
| `ch11_m11_11` | 1.87x | 0.0089 | 3.58 |
| `sw_mi` | 1.83x | 0.0131 | 3.88 |
| `ch13_m13_2` | 1.83x | 0.00882 | 3.15 |
| `ch13_m13_3` | 1.83x | 0.00848 | 3.04 |
| `ctsem_ctsm` | 1.81x | 3.29 | 93.3 |
| `sw_ma` | 1.81x | 0.0141 | 4.28 |
| `sw_arma` | 1.81x | 0.0158 | 4.36 |
| `sw_ar` | 1.81x | 0.0143 | 4.27 |
| `ctsem_ctsm_400` | 1.79x | 27.6 | 137 |
| `sw_asymlaplace` | 1.79x | 0.0139 | 3.59 |
| `sw_zi_binomial` | 1.78x | 0.0165 | 3.43 |
| `wells_dist` | 1.78x | 0.057 | 3.04 |
| `ch13_m13_1` | 1.77x | 0.00843 | 2.98 |
| `sw_beta` | 1.75x | 0.0124 | 3.91 |
| `cm_weibull` | 1.73x | 0.0379 | 4.09 |
| `cm_invweibull` | 1.73x | 0.0395 | 4.08 |
| `s2_mv_subset` | 1.71x | 0.0127 | 3.5 |
| `sw_gaussian` | 1.69x | 0.00942 | 3.32 |
| `ch12_m12_6` | 1.69x | 4.17 | 10.9 |
| `surgical_model` | 1.69x | 0.00866 | 3.44 |
| `ch11_m11_7` | 1.69x | 0.0072 | 2.86 |
| `cm_logstudent` | 1.68x | 0.0388 | 4.22 |
| `cm_choco` | 1.67x | 0.115 | 4.67 |
| `s2_unstr` | 1.66x | 0.0392 | 7.56 |
| `cm_invgamma` | 1.66x | 0.0404 | 4.11 |
| `s2_zoi_beta` | 1.66x | 0.0229 | 3.71 |
| `s2_zi_beta` | 1.64x | 0.0213 | 3.67 |
| `sw_mv_norescor` | 1.63x | 0.0127 | 3.49 |
| `sw_mv_rescor` | 1.63x | 0.024 | 6.29 |
| `s2_categorical_re` | 1.63x | 0.026 | 5.48 |
| `cm_loggamma` | 1.6x | 0.046 | 4.24 |
| `cm_lognormal` | 1.6x | 0.0776 | 4.29 |
| `GLM_Binomial_model` | 1.59x | 0.00912 | 3.29 |
| `cm_gamma` | 1.59x | 0.0385 | 4.09 |
| `cm_lnr_bench` | 1.59x | 0.564 | 5.27 |
| `sw_zi_negbin` | 1.58x | 0.0187 | 3.49 |
| `cm_bisa` | 1.58x | 0.0379 | 4.08 |
| `Rate_4_model` | 1.56x | 0.0064 | 2.49 |
| `cm_betagate` | 1.55x | 0.0865 | 4.1 |
| `s2_frechet` | 1.54x | 0.0127 | 3.99 |
| `cm_logweibull` | 1.53x | 0.0368 | 4.06 |
| `aalto_binom` | 1.52x | 0.0058 | 2.38 |
| `covid19imperial_v2` | 1.52x | 1.35 | 7.72 |
| `ch14_m14_6` | 1.51x | 0.187 | 6.95 |
| `gp_pois_regr` | 1.51x | 0.0115 | 5.82 |
| `cm_lba2` | 1.5x | 0.0892 | 4.72 |
| `covid19imperial_v3` | 1.5x | 1.35 | 7.73 |
| `sw_re_negbin` | 1.5x | 0.0152 | 4.78 |
| `s2_gp_by_gr` | 1.49x | 0.0425 | 9.18 |
| `ch14_m14_6x` | 1.49x | 0.187 | 6.95 |
| `s2_mixture_theta` | 1.49x | 0.0279 | 4.6 |
| `Mb_model` | 1.49x | 0.112 | 3.39 |
| `ch11_m11_9` | 1.47x | 0.00687 | 2.61 |
| `sw_weibull` | 1.46x | 0.0134 | 3.93 |
| `Rate_1_model` | 1.46x | 0.00654 | 2.38 |
| `cm_lba1` | 1.45x | 0.0603 | 4.41 |
| `Rate_3_model` | 1.44x | 0.00612 | 2.4 |
| `aalto_binomb` | 1.42x | 0.00618 | 2.41 |
| `s2_zi_asymlaplace` | 1.42x | 0.0183 | 3.62 |
| `Rate_5_model` | 1.41x | 0.00636 | 2.49 |
| `aalto_bern` | 1.41x | 0.00696 | 2.57 |
| `Rate_2_model` | 1.4x | 0.0071 | 2.58 |
| `hmm_example` | 1.4x | 0.0576 | 4.17 |
| `ch14_m14_2` | 1.4x | 0.0472 | 7.15 |
| `ch12_m12_2` | 1.38x | 0.00958 | 4.02 |
| `gp_regr` | 1.38x | 0.0122 | 5.48 |
| `aalto_binom2` | 1.38x | 0.0069 | 2.49 |
| `ch11_m_pois` | 1.35x | 0.00637 | 2.53 |
| `cm_exwald` | 1.35x | 0.114 | 4.33 |
| `ch12_m12_1` | 1.34x | 0.0092 | 3.24 |
| `i320_gp_expquad` | 1.33x | 0.0337 | 7.11 |
| `sw_mono` | 1.33x | 0.0167 | 4 |
| `cm_rdm` | 1.31x | 0.226 | 4.57 |
| `sw_binomial` | 1.31x | 0.0119 | 3.56 |
| `sw_sratio` | 1.31x | 0.141 | 3.8 |
| `s2_mo_simo_prior` | 1.3x | 0.0161 | 4 |
| `i320_sratio_plain` | 1.3x | 0.144 | 3.96 |
| `sw_cratio` | 1.29x | 0.142 | 3.8 |
| `ch14_m14_8nc` | 1.29x | 0.0174 | 5.74 |
| `sw_mixture` | 1.29x | 0.0244 | 4.82 |
| `i319_negbin_re` | 1.27x | 0.0304 | 4.81 |
| `s2_beta_binomial` | 1.27x | 0.0183 | 3.75 |
| `sw_gp` | 1.26x | 0.0494 | 7.14 |
| `sw_bernoulli` | 1.26x | 0.00874 | 3.29 |
| `ch14_m14_1` | 1.25x | 0.021 | 6.99 |
| `nn_rbm1bJ10` | 1.25x | 0.341 | 5.91 |
| `sw_cratio_cs` | 1.21x | 0.209 | 4.97 |
| `ch14_m14_9` | 1.2x | 0.728 | 4.95 |
| `i320_sratio_cs` | 1.2x | 0.211 | 5.29 |
| `s2_custom_vint` | 1.2x | 0.02 | 3.59 |
| `ch14_m14_10` | 1.2x | 0.765 | 4.99 |
| `sw_categorical` | 1.16x | 0.0132 | 4.33 |
| `s2_cumulative_probit` | 1.16x | 0.0188 | 3.73 |
| `ch14_m14_7` | 1.16x | 0.0761 | 8.99 |
| `s2_multinomial` | 1.14x | 0.0308 | 4.04 |
| `iohmm_reg` | 1.13x | 0.885 | 6.83 |
| `Survey_model` | 1.13x | 0.126 | 3.09 |
| `bones_model` | 1.11x | 0.123 | 3.65 |
| `ch14_m14_8` | 1.1x | 0.0154 | 4.52 |
| `one_comp_mm_elim_abs` | 1.09x | 1.01 | 4.57 |
| `s2_discrete_weibull` | 1.08x | 0.0171 | 3.49 |
| `sw_trunc` | 1.08x | 0.0225 | 3.62 |
| `cm_lnr` | 1.07x | 0.309 | 4.69 |
| `arma11` | 1.06x | 0.0203 | 3 |
| `s2_hurdle_negbin` | 1.06x | 0.0265 | 3.5 |
| `s2_dirichlet` | 1.06x | 0.0388 | 4.43 |
| `garch11` | 1.05x | 0.0254 | 2.97 |
| `s2_weights_trunc` | 1.05x | 0.0239 | 3.63 |
| `nn_rbm1bJ100` | 1.05x | 869 | 920 |
| `aalto_poisson_simple` | 1.04x | 0.0103 | 2.66 |
| `sw_negbinomial` | 1.04x | 0.012 | 3.4 |
| `nes_logit_model` | 1.04x | 0.02 | 3.18 |
| `diamonds` | 1.03x | 0.0951 | 3.52 |
| `wells_interaction_model` | 1.03x | 0.049 | 3.31 |
| `hmm_drive_1` | 1.03x | 0.355 | 5.13 |
| `wells_dae_c_model` | 1.02x | 0.0482 | 3.43 |
| `i319_negbin_fixed` | 1.02x | 0.0257 | 3.41 |
| `wells_daae_c_model` | 1.02x | 0.0503 | 3.42 |
| `i320_pois_trunc_both` | 1.02x | 0.0748 | 3.64 |
| `wells_dist100_model` | 1.01x | 0.0428 | 3.24 |
| `sw_cumulative_cs` | 1.01x | 0.187 | 4.91 |
| `i320_pois_trunc_ub` | 1x | 0.0603 | 3.57 |
| `sw_cumulative` | 1x | 0.054 | 3.84 |
| `wells_dae_inter_model` | 0.995x | 0.0521 | 3.52 |
| `s2_wiener` | 0.99x | 0.0741 | 3.76 |
| `cm_betadiscrete` | 0.99x | 3.98 | 8.09 |
| `wells_dist100ars_model` | 0.986x | 0.0475 | 3.26 |
| `wells_dae_model` | 0.984x | 0.0506 | 3.27 |
| `hmm_gaussian` | 0.984x | 0.739 | 5.39 |
| `cm_ddm` | 0.984x | 41.4 | 47.2 |
| <code>wells_interaction_c_</code><br><code>model</code> | 0.983x | 0.0506 | 3.4 |
| `hmm_drive_0` | 0.98x | 0.344 | 5.13 |
| `ch15_m15_7` | 0.978x | 0.0399 | 7.24 |
| `i320_gp_matern32` | 0.931x | 0.0732 | 7.13 |
| `ch14_m14_11` | 0.923x | 1.53 | 5.39 |
| `s2_logistic_normal` | 0.877x | 0.0696 | 6.42 |
| `aalto_gpareto` | 0.863x | 0.0134 | 2.93 |
| `s2_ar_cov` | 0.862x | 0.0349 | 7.29 |
| `s2_cosy` | 0.861x | 0.0339 | 6.55 |
| `s2_fcor` | 0.805x | 0.0426 | 4.97 |
| `s2_com_poisson` | 0.263x | 0.496 | 3.76 |
| `cm_invgaussian` | 0.049x | 2.1 | 4.82 |

Each estimate is the engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli gradient | Stanli estimate (s) | CmdStan estimate (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2797) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5360) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6075) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->

## Fast mode

[Fast mode](fast-mode.md) uses the same frozen runtime, inputs and protocol as the default run, with `corpus_bench.py --fast-math`. All 343 application models were attempted; 337 have complete accepted timings.

Estimated corpus time: **Stanli 982.3 s vs CmdStan 2,604.4 s (2.65x CmdStan / Stanli)**, using setup + 2,000 gradients per model for the same 337/343 models. Six incomplete models leave the full-corpus total unavailable.

Across the 337 models with accepted default and fast-mode timings, fast mode's warm gradients are 1.93x faster in geometric mean and 1.04x faster at the median model. 79 models are at least 2x faster, 41 at least 10x and 8 at least 100x. These improvements have little effect on the corpus total because costly models and setup dominate the summed time.

Against CmdStan, the geometric mean of paired gradient speedups is 2.23x in default mode and 4.28x in fast mode. Default mode is at or above parity on 318/337 models, and fast mode on 325/337. The default and fast sweeps ran serially; 31 models timed at least 2% slower in fast mode. These differences have not been confirmed by controlled alternating default/fast experiments and remain unresolved; this run makes no per-model no-regression claim.

The largest measured gradient gains, in microseconds:

| Model | Default | Fast | Default / fast |
| --- | ---: | ---: | ---: |
| `radon_pooled` | 41.1 | 0.0821 | 500x |
| `nes` | 18.7 | 0.0737 | 254x |
| `logearn_interaction` | 7.81 | 0.0506 | 154x |
| `logearn_interaction_z` | 8.01 | 0.052 | 154x |
| `logearn_logheight_male` | 5.57 | 0.0439 | 127x |
| `logearn_height_male` | 5.6 | 0.0459 | 122x |
| `diamonds` | 35.4 | 0.313 | 113x |
| `ch12_m12_4` | 67.7 | 0.656 | 103x |

The [fast-mode evidence](../output/corpus-performance-fast/README.md) retains every model, raw observations, numerical failures and build identity.

## Appendix

The [main table](#full-corpus) uses default CmdStan compiler flags.
The appendix table below uses stanc3 `--O1` with loop vectorization enabled.
Both experiments cover the same application inputs with the same Stanli build.

### Configuration and evidence

| Item | Configuration |
| --- | --- |
| Scope | All 343 application models, `--corpus all` |
| Gradient trials | Six alternating pairs, 200 ms warmup and 250 ms measurement per process |
| Displayed workload | Setup + 2,000 × median warm gradient time |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled, lite mode disabled |
| Measured source | Frozen `bd6f15290b23851b744e244960a6e14a29ed87fb` plus [retained local changes](../output/corpus-performance/source-provenance.json); later worktree changes are unmeasured |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan source | `11cb052d3e1fc8c799e0fec559e2ee5452b38d27` |
| Stanli Math | `5252d51d47c1d5e78005fc043ad996fad6dd8da8` |
| CmdStan Math | `8f326d14599d3030c626c46532d8e8534c1cdbec` |
| Compiler / deployment target | AppleClang 21, macOS 26.0; host benchmark validation |
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

**Per-model fixed-work estimate:** each engine's setup time plus 2,000 times its
median warm gradient time. No sampler is run, and adaptation, tree building,
generated quantities and CSV output are excluded. The retained v4 summaries
and manifests use a 20,000-gradient budget; the tables on this page recalculate
totals for 2,000 gradients from the same validated setup and latency measurements.
A missing setup component leaves that estimate blank without hiding a valid
gradient measurement in the incomplete-results table. Sampler correctness tests
remain part of [the validation suite](../TESTING.md).

The fixed-point gate leaves six default and fast-mode rows without estimates. `dogs_log`, `s2_invgaussian` and `sir` report non-finite density or gradients. `kronecker_gp`, `s2_mi_trunc_lb` and `sw_cens` fail the numerical gate at scaled errors `0.000113`, `2.99e-7` and `2.44e-6`, respectively. The optimized-reference run also rejects `s2_gp_by_gr` at `5.33e-8`. These are the same failures as the preceding published runs. The incomplete-results tables preserve them; see the [numerical policies and exceptions](../TESTING.md#comparison-with-cmdstan-on-complete-models).

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
Run `a8ce0ee41414bcf4`: 343 models, 6 alternating gradient pairs.

Estimated corpus time: Stanli 989.4 s vs CmdStan 3,369.0 s (3.40x CmdStan / Stanli; 336/343 models). Setup + 2,000 gradients per model. Full-corpus total unavailable: 7 models have incomplete measurements.

| Model | CmdStan / Stanli gradient | Stanli estimate (s) | CmdStan estimate (s) |
| --- | ---: | ---: | ---: |
| `ch12_m12_4` | 39.6x | 0.159 | 8.68 |
| `gpcm_latent_reg_irt` | 15.3x | 0.341 | 9.1 |
| `grsm_latent_reg_irt` | 13.6x | 0.174 | 7.22 |
| <code>radon_partially_pooled_</code><br><code>centered</code> | 11.2x | 0.0911 | 3.62 |
| <code>radon_partially_pooled_</code><br><code>noncentered</code> | 10.9x | 0.0937 | 3.79 |
| <code>radon_variable_</code><br><code>intercept_centered</code> | 8.26x | 0.148 | 3.82 |
| <code>radon_variable_slope_</code><br><code>noncentered</code> | 8.07x | 0.148 | 4.04 |
| `aalto_poisson_hurdle` | 8.07x | 0.0405 | 3.07 |
| <code>radon_variable_slope_</code><br><code>centered</code> | 8.07x | 0.149 | 3.84 |
| `radon_county_intercept` | 8.04x | 0.144 | 3.73 |
| <code>radon_variable_</code><br><code>intercept_noncentered</code> | 7.91x | 0.147 | 4.04 |
| <code>radon_variable_</code><br><code>intercept_slope_centered</code> | 7.53x | 0.162 | 3.99 |
| <code>radon_hierarchical_</code><br><code>intercept_centered</code> | 7.44x | 0.212 | 4.23 |
| <code>radon_variable_</code><br><code>intercept_slope_</code><br><code>noncentered</code> | 7.43x | 0.162 | 4.29 |
| <code>radon_hierarchical_</code><br><code>intercept_noncentered</code> | 7.32x | 0.225 | 4.41 |
| `ch12_m12_3_alt` | 7.31x | 0.0141 | 2.54 |
| `s2_custom_vreal` | 6.88x | 0.0104 | 3.47 |
| `ch16_m16_1` | 6.82x | 0.0205 | 3.08 |
| `rats_model` | 6.75x | 0.00971 | 3.13 |
| `ch12_m12_3` | 6.72x | 0.014 | 2.53 |
| `M0_model` | 6.44x | 0.0147 | 2.74 |
| `Mt_model` | 5.87x | 0.0171 | 3.49 |
| `arK` | 4.94x | 0.0124 | 2.89 |
| `Mth_model` | 4.52x | 0.0665 | 4.52 |
| `s2_car_icar` | 4.25x | 0.0116 | 4.43 |
| `s2_cumulative_cauchit` | 4.08x | 0.0152 | 3.81 |
| `s2_gp_by_approx` | 3.77x | 0.0197 | 7.09 |
| `s2_cox_cens` | 3.63x | 0.0145 | 5.05 |
| `s2_hurdle_cumulative` | 3.61x | 0.0184 | 4.2 |
| `sw_weights` | 3.57x | 0.0107 | 3.48 |
| `dogs` | 3.51x | 0.0427 | 4.26 |
| `s2_mm` | 3.47x | 0.0139 | 4.73 |
| `s2_mm_weights` | 3.45x | 0.0128 | 4.73 |
| `cm_exgaussian` | 3.44x | 0.0422 | 4.45 |
| `GLMM1_model` | 3.43x | 0.0302 | 3.29 |
| `sw_me` | 3.4x | 0.012 | 4.3 |
| `ch15_m15_5` | 3.3x | 0.00964 | 3.69 |
| `i319_pois_re2` | 3.25x | 0.0213 | 4.89 |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.25x | 0.022 | 4.93 |
| `s2_me2_nomecor` | 3.24x | 0.0142 | 4.58 |
| `i319_gauss_re` | 3.23x | 0.0155 | 4.61 |
| `s2_gr_by` | 3.23x | 0.0136 | 4.99 |
| `aalto_grp_aov` | 3.22x | 0.00677 | 3.06 |
| `s2_car_esicar` | 3.21x | 0.0142 | 4.79 |
| `s2_dist_sigma_re` | 3.19x | 0.0135 | 5.02 |
| `logmesquite` | 3.13x | 0.00934 | 3.42 |
| `logmesquite_logvolume` | 3.12x | 0.00826 | 3.06 |
| `sw_re_gauss` | 3.11x | 0.0128 | 4.63 |
| `ch12_m12_5` | 3.08x | 2.06 | 9.54 |
| `sw_acat` | 3.07x | 0.111 | 4.25 |
| `ch12_m12_7` | 3.07x | 2.02 | 9.38 |
| `eight_schools_centered` | 3.06x | 0.00702 | 3.01 |
| `sw_acat_cs` | 3.02x | 0.158 | 5.52 |
| `logmesquite_logvash` | 3.02x | 0.00948 | 3.57 |
| `i319_pois_re` | 3.02x | 0.0165 | 4.55 |
| `ch09_m9_1_chains4` | 3.01x | 0.00951 | 3.3 |
| `mesquite` | 3.01x | 0.00863 | 3.38 |
| `radon_county` | 3.01x | 0.0648 | 3.27 |
| `ch09_m9_1` | 3x | 0.009 | 3.31 |
| `s2_car` | 3x | 0.014 | 4.74 |
| `logmesquite_logvas` | 2.99x | 0.00989 | 3.65 |
| `sw_re_pois` | 2.95x | 0.0119 | 4.55 |
| `logmesquite_logva` | 2.91x | 0.00867 | 3.35 |
| `Mtbh_model` | 2.89x | 0.0603 | 5.03 |
| `aalto_grp_prior_mean` | 2.87x | 0.00756 | 3.12 |
| `s2_gr_student` | 2.83x | 0.0143 | 5.35 |
| `cm_geg` | 2.82x | 0.0622 | 4.7 |
| `ch15_m15_1` | 2.81x | 0.00842 | 3.42 |
| `s2_cox` | 2.81x | 0.0134 | 4.13 |
| `sw_hurdle_lognormal` | 2.78x | 0.0154 | 3.59 |
| `ch09_m5_8s` | 2.77x | 0.00852 | 3.06 |
| `aalto_lin_std` | 2.76x | 0.00793 | 3.25 |
| `aalto_lin` | 2.74x | 0.0081 | 3.04 |
| <code>eight_schools_</code><br><code>noncentered</code> | 2.72x | 0.00686 | 3.22 |
| `dogs_nonhierarchical` | 2.71x | 0.0595 | 7 |
| `aalto_grp_prior_mean_var` | 2.67x | 0.008 | 3.86 |
| `s2_gp_approx` | 2.67x | 0.0147 | 5.56 |
| `ch15_m15_2` | 2.63x | 0.0101 | 3.67 |
| `ldaK5` | 2.62x | 5.31 | 15.9 |
| `ch15_m15_9` | 2.6x | 0.0141 | 3.14 |
| `pilots` | 2.56x | 0.00957 | 3.5 |
| `ch09_m5_8s2` | 2.55x | 0.00831 | 3.06 |
| `sw_hurdle_pois` | 2.54x | 0.0159 | 3.43 |
| `kilpisjarvi` | 2.53x | 0.00793 | 2.81 |
| `s2_shifted_lognormal` | 2.52x | 0.0102 | 3.93 |
| `ch15_m15_8` | 2.5x | 0.0137 | 2.9 |
| `hierarchical_gp` | 2.49x | 0.0629 | 9.25 |
| `nes` | 2.48x | 0.0493 | 3.7 |
| `s2_nlf` | 2.48x | 0.0112 | 4.38 |
| `ch15_m15_6` | 2.44x | 0.00781 | 3.2 |
| `s2_s_cc` | 2.43x | 0.0108 | 4.25 |
| `sw_skewnormal` | 2.42x | 0.0115 | 3.92 |
| `ch09_m9_2` | 2.41x | 0.00678 | 2.55 |
| `sw_spline_s` | 2.41x | 0.0111 | 4.39 |
| `accel_gp` | 2.4x | 0.0288 | 6.67 |
| `losscurve_sislob` | 2.39x | 0.016 | 4.71 |
| `ch14_m14_3` | 2.37x | 0.0355 | 5.97 |
| `s2_cumulative_cloglog` | 2.37x | 0.0138 | 3.8 |
| `extra_hurdle_poisson` | 2.37x | 0.00747 | 2.61 |
| `sw_re_bern` | 2.37x | 0.0113 | 4.68 |
| `i320_mi_nhanes` | 2.36x | 0.0166 | 4.46 |
| `s2_mmc` | 2.35x | 0.0177 | 7.07 |
| `ch09_m9_3` | 2.35x | 0.00626 | 2.54 |
| `normal_mixture` | 2.34x | 0.0934 | 3.02 |
| `kidscore_interaction` | 2.33x | 0.0129 | 3.22 |
| `s2_sar` | 2.32x | 0.0181 | 4.23 |
| `kidscore_interaction_c` | 2.3x | 0.013 | 3.21 |
| `kidscore_mom_work` | 2.29x | 0.014 | 3.12 |
| `ch09_m9_5` | 2.28x | 0.00701 | 2.55 |
| `ch14_m14_5` | 2.28x | 0.0117 | 3.05 |
| `kidscore_interaction_c2` | 2.27x | 0.0127 | 3.24 |
| `ch11_m11_5` | 2.27x | 0.0273 | 3.13 |
| `Mh_model` | 2.25x | 0.0473 | 3.39 |
| `ch09_m9_4` | 2.24x | 0.00672 | 2.56 |
| `ch11_m11_10` | 2.24x | 0.00846 | 3.25 |
| `sw_nonlinear` | 2.24x | 0.0103 | 3.94 |
| `dogs_hierarchical` | 2.23x | 0.0554 | 2.86 |
| `ch13_m13_6` | 2.22x | 0.0286 | 3.48 |
| `kidscore_interaction_z` | 2.22x | 0.0131 | 3.27 |
| `s2_me2` | 2.21x | 0.0172 | 6.74 |
| `sw_vonmises` | 2.21x | 0.0107 | 3.66 |
| `s2_sar_error` | 2.21x | 0.0179 | 4.21 |
| `logearn_height_male` | 2.21x | 0.0187 | 3.08 |
| `ldaK2` | 2.2x | 0.12 | 3.89 |
| `low_dim_gauss_mix` | 2.2x | 0.104 | 3.45 |
| <code>low_dim_gauss_mix_</code><br><code>collapse</code> | 2.2x | 0.103 | 3.35 |
| `dugongs_model` | 2.19x | 0.00832 | 3.05 |
| `logearn_interaction_z` | 2.19x | 0.0237 | 3.32 |
| `logearn_logheight_male` | 2.19x | 0.0188 | 3.09 |
| `aalto_lin_std_t` | 2.19x | 0.00862 | 3.47 |
| `ch13_m13_4b` | 2.18x | 0.0236 | 3.35 |
| `s2_mv_shared_re` | 2.18x | 0.0199 | 7.2 |
| `s2_mi_lognormal` | 2.16x | 0.0129 | 4.56 |
| `GLMM_Poisson_model` | 2.16x | 0.00955 | 3.85 |
| `kidscore_momhsiq` | 2.15x | 0.0117 | 3.08 |
| `ch13_m13_4` | 2.15x | 0.0282 | 3.47 |
| `s2_s_by` | 2.14x | 0.0239 | 5.76 |
| `sw_lognormal` | 2.13x | 0.0101 | 3.66 |
| `ch13_m13_7` | 2.13x | 0.0059 | 2.29 |
| `ch13_m13_4nc` | 2.13x | 0.0291 | 3.36 |
| `s2_nl_noloop` | 2.12x | 0.0104 | 3.84 |
| `seeds_centered_model` | 2.12x | 0.00949 | 3.94 |
| `sesame_one_pred_a` | 2.11x | 0.00915 | 2.86 |
| `logearn_interaction` | 2.1x | 0.0239 | 3.22 |
| `ctsem_ctsm` | 2.09x | 3.29 | 445 |
| `radon_pooled` | 2.07x | 0.092 | 3.06 |
| `logearn_height` | 2.07x | 0.0151 | 2.94 |
| `s2_gev` | 2.06x | 0.0168 | 3.85 |
| `lotka_volterra` | 2.06x | 0.0572 | 4.6 |
| `ctsem_ctsm_400` | 2.05x | 26.7 | 490 |
| `earn_height` | 2.04x | 0.0146 | 2.87 |
| `kidscore_momiq` | 2.03x | 0.00961 | 2.93 |
| `accel_splines` | 2.03x | 0.0216 | 4.65 |
| `ch14_m14_4` | 2.03x | 0.0107 | 2.87 |
| `log10earn_height` | 2.03x | 0.0147 | 2.91 |
| `sw_se` | 2.03x | 0.00977 | 3.49 |
| `sw_dist_sigma` | 2.03x | 0.0118 | 3.78 |
| `ch11_m11_6` | 2.03x | 0.00899 | 3.03 |
| `soil_incubation` | 2.02x | 0.0766 | 4.08 |
| `GLM_Poisson_model` | 2.01x | 0.00849 | 3.3 |
| `ch13_m13_5` | 2.01x | 0.0258 | 3.33 |
| `election88_full` | 2.01x | 0.492 | 6.05 |
| `ch11_m11_8` | 2.01x | 0.00759 | 2.94 |
| `sw_re_slope` | 2.01x | 0.0172 | 6.89 |
| `ch11_m11_4` | 2x | 0.0246 | 3.05 |
| `kidscore_momhs` | 1.99x | 0.0101 | 2.93 |
| `sw_student` | 1.99x | 0.00998 | 3.81 |
| `sw_zi_poisson` | 1.98x | 0.0147 | 3.43 |
| `ch14_m14_4x` | 1.98x | 0.0106 | 2.88 |
| `ch09_mp` | 1.97x | 0.00579 | 2.3 |
| `sw_asymlaplace` | 1.96x | 0.0148 | 3.65 |
| `i319_pois_fixed` | 1.94x | 0.0114 | 3.14 |
| `normal_mixture_k` | 1.93x | 0.432 | 4.2 |
| `ch13_m13_7nc` | 1.91x | 0.00635 | 2.38 |
| `sw_mi` | 1.91x | 0.0125 | 3.87 |
| `ch11_m11_11` | 1.89x | 0.00897 | 3.54 |
| `ch13_m13_1` | 1.88x | 0.00907 | 3 |
| `sw_spline_t2` | 1.87x | 0.0146 | 4.88 |
| `s2_index_mi` | 1.87x | 0.0142 | 4.21 |
| `s2_t2_by` | 1.87x | 0.0208 | 5.73 |
| `ch16_m16_4` | 1.86x | 0.0109 | 3.35 |
| `multi_occupancy` | 1.84x | 0.0851 | 6.63 |
| `s2_cens_interval` | 1.84x | 0.0133 | 3.76 |
| `ch13_m13_3` | 1.84x | 0.00843 | 3.05 |
| `sw_poisson` | 1.81x | 0.0092 | 3.13 |
| `bym2_offset_only` | 1.77x | 0.0848 | 4.5 |
| `ch13_m13_2` | 1.76x | 0.00901 | 3.14 |
| `seeds_stanified_model` | 1.76x | 0.00971 | 3.66 |
| `sw_hurdle_gamma` | 1.75x | 0.0166 | 3.55 |
| `ch15_m15_4` | 1.75x | 0.0303 | 2.69 |
| `cm_loggamma` | 1.74x | 0.0454 | 4.66 |
| `ch15_m15_3` | 1.73x | 0.0353 | 2.68 |
| `ch11_m11_7` | 1.71x | 0.00693 | 2.85 |
| `s2_unstr` | 1.69x | 0.0371 | 8.81 |
| `s2_threading` | 1.69x | 0.00986 | 3.41 |
| `cm_gamma` | 1.69x | 0.0376 | 4.18 |
| `cm_lognormal` | 1.69x | 0.0734 | 5.53 |
| `seeds_model` | 1.68x | 0.00963 | 3.74 |
| `sw_mv_rescor` | 1.68x | 0.0227 | 6.17 |
| `sw_zi_binomial` | 1.67x | 0.0177 | 3.47 |
| `surgical_model` | 1.66x | 0.00815 | 3.44 |
| `cm_invweibull` | 1.65x | 0.0401 | 4.11 |
| `cm_weibull` | 1.65x | 0.0367 | 4.13 |
| `sw_ma` | 1.65x | 0.0144 | 4.27 |
| `hier_2pl` | 1.64x | 0.413 | 7.47 |
| `cm_logstudent` | 1.62x | 0.0396 | 4.24 |
| `cm_invgamma` | 1.61x | 0.0386 | 4.1 |
| `sw_arma` | 1.61x | 0.0163 | 4.35 |
| `sw_cratio_cs` | 1.61x | 0.203 | 5.34 |
| `cm_betagate` | 1.6x | 0.0836 | 4.31 |
| `sw_ar` | 1.6x | 0.0137 | 4.25 |
| `sw_gaussian` | 1.59x | 0.009 | 3.31 |
| `sw_re_negbin` | 1.59x | 0.0152 | 4.82 |
| `cm_logweibull` | 1.57x | 0.0362 | 4.1 |
| `sw_zi_negbin` | 1.57x | 0.0186 | 3.5 |
| `cm_bisa` | 1.57x | 0.0384 | 4.22 |
| `2pl_latent_reg_irt` | 1.57x | 0.153 | 5.67 |
| `i320_sratio_cs` | 1.56x | 0.208 | 5.66 |
| `s2_categorical_re` | 1.56x | 0.0265 | 5.45 |
| `sw_mv_norescor` | 1.56x | 0.0118 | 3.49 |
| `ch12_m12_6` | 1.54x | 4.13 | 10.4 |
| `ch11_m11_9` | 1.54x | 0.00698 | 2.61 |
| `cm_lnr_bench` | 1.54x | 0.547 | 9.06 |
| `Mb_model` | 1.53x | 0.113 | 3.39 |
| `aalto_binom` | 1.52x | 0.00625 | 2.39 |
| `cm_lba1` | 1.51x | 0.06 | 4.96 |
| `Rate_4_model` | 1.51x | 0.00678 | 2.5 |
| `cm_choco` | 1.5x | 0.114 | 5.19 |
| `sw_cumulative_cs` | 1.5x | 0.184 | 5.25 |
| `Rate_1_model` | 1.5x | 0.00582 | 2.38 |
| `s2_mv_subset` | 1.5x | 0.012 | 3.5 |
| `lsat_model` | 1.49x | 0.0941 | 3.89 |
| `sw_gamma` | 1.49x | 0.0103 | 3.79 |
| `s2_zoi_beta` | 1.49x | 0.0232 | 3.8 |
| `covid19imperial_v3` | 1.48x | 1.35 | 7.65 |
| `s2_rate` | 1.47x | 0.0094 | 3.6 |
| `prophet` | 1.47x | 0.0915 | 5.72 |
| `ch14_m14_6x` | 1.45x | 0.187 | 7.03 |
| `covid19imperial_v2` | 1.44x | 1.34 | 7.6 |
| `ch14_m14_6` | 1.44x | 0.185 | 7.05 |
| `Rate_5_model` | 1.42x | 0.00707 | 2.51 |
| `s2_zi_beta` | 1.42x | 0.0208 | 3.76 |
| `gp_pois_regr` | 1.42x | 0.0116 | 5.71 |
| `hmm_example` | 1.41x | 0.0589 | 4.18 |
| `sw_exgaussian` | 1.41x | 0.0115 | 3.87 |
| `Rate_2_model` | 1.41x | 0.00702 | 2.57 |
| `aalto_bern` | 1.4x | 0.00671 | 2.58 |
| `cm_lba2` | 1.4x | 0.0911 | 7.5 |
| `irt_2pl` | 1.4x | 0.0424 | 4.3 |
| `s2_zi_asymlaplace` | 1.4x | 0.0189 | 3.75 |
| `Rate_3_model` | 1.39x | 0.00608 | 2.41 |
| `ch14_m14_2` | 1.39x | 0.0455 | 7.15 |
| `aalto_binom2` | 1.39x | 0.00728 | 2.49 |
| `s2_mixture_theta` | 1.38x | 0.0269 | 4.46 |
| `s2_cumulative_probit` | 1.37x | 0.0184 | 3.72 |
| `ch11_m_pois` | 1.37x | 0.00617 | 2.53 |
| `aalto_binomb` | 1.37x | 0.00602 | 2.41 |
| `ch12_m12_2` | 1.37x | 0.00974 | 4.01 |
| `cm_exwald` | 1.36x | 0.118 | 6.01 |
| `ch12_m12_1` | 1.36x | 0.00955 | 3.22 |
| `blr` | 1.36x | 0.00762 | 3.05 |
| `gp_regr` | 1.35x | 0.0119 | 5.51 |
| `i319_negbin_re` | 1.33x | 0.0308 | 4.85 |
| `i320_gp_expquad` | 1.32x | 0.0328 | 7.36 |
| `sw_mixture` | 1.31x | 0.0239 | 4.77 |
| `wells_dist` | 1.31x | 0.0575 | 3.08 |
| `GLM_Binomial_model` | 1.3x | 0.00939 | 3.3 |
| `ch14_m14_9` | 1.28x | 0.683 | 5.08 |
| `sw_gp` | 1.26x | 0.0507 | 7.38 |
| `nn_rbm1bJ10` | 1.26x | 0.331 | 5.86 |
| `ch14_m14_8nc` | 1.26x | 0.0176 | 6.01 |
| `sw_beta` | 1.25x | 0.0124 | 3.92 |
| `s2_frechet` | 1.24x | 0.0136 | 3.96 |
| `ch14_m14_10` | 1.22x | 0.762 | 5.15 |
| `s2_custom_vint` | 1.22x | 0.0199 | 3.57 |
| `i320_sratio_plain` | 1.2x | 0.144 | 4.25 |
| `sw_bernoulli` | 1.2x | 0.00874 | 3.32 |
| `logistic_regression_rhs` | 1.19x | 0.108 | 5.16 |
| `sw_cratio` | 1.18x | 0.141 | 4.04 |
| `sw_sratio` | 1.18x | 0.142 | 4.08 |
| `sw_categorical` | 1.17x | 0.0119 | 4.41 |
| `cm_rdm` | 1.16x | 0.243 | 10.9 |
| `ch14_m14_1` | 1.16x | 0.0213 | 6.94 |
| `ch14_m14_8` | 1.14x | 0.0155 | 4.76 |
| `ch14_m14_7` | 1.13x | 0.0769 | 8.97 |
| `s2_dirichlet` | 1.13x | 0.0385 | 4.39 |
| `sw_weibull` | 1.12x | 0.0123 | 3.94 |
| `Survey_model` | 1.11x | 0.132 | 3.12 |
| `sw_binomial` | 1.11x | 0.0117 | 3.53 |
| `s2_beta_binomial` | 1.09x | 0.0184 | 3.73 |
| `sw_trunc` | 1.09x | 0.0227 | 3.56 |
| `s2_weights_trunc` | 1.09x | 0.0227 | 3.59 |
| `s2_multinomial` | 1.08x | 0.0317 | 3.98 |
| `bones_model` | 1.08x | 0.124 | 3.62 |
| `sw_negbinomial` | 1.06x | 0.0118 | 3.42 |
| `aalto_poisson_simple` | 1.05x | 0.00986 | 2.66 |
| `nn_rbm1bJ100` | 1.05x | 877 | 925 |
| `i319_negbin_fixed` | 1.05x | 0.0253 | 3.44 |
| <code>wells_interaction_c_</code><br><code>model</code> | 1.05x | 0.0486 | 3.38 |
| `nes_logit_model` | 1.04x | 0.0202 | 3.18 |
| `s2_hurdle_negbin` | 1.04x | 0.0268 | 3.52 |
| `wells_dae_c_model` | 1.04x | 0.0465 | 3.42 |
| `wells_dae_inter_model` | 1.04x | 0.0524 | 3.4 |
| `one_comp_mm_elim_abs` | 1.03x | 1.03 | 4.87 |
| `s2_discrete_weibull` | 1.03x | 0.0173 | 3.5 |
| `sw_mono` | 1.03x | 0.0153 | 4.16 |
| `s2_mo_simo_prior` | 1.03x | 0.0165 | 4.13 |
| `hmm_drive_1` | 1.02x | 0.355 | 5.13 |
| `cm_lnr` | 1.02x | 0.307 | 14.1 |
| `cm_ddm` | 1.01x | 39.1 | 48.6 |
| `wells_interaction_model` | 1.01x | 0.0492 | 3.34 |
| `wells_dist100ars_model` | 1.01x | 0.0453 | 3.26 |
| `ch15_m15_7` | 1.01x | 0.0387 | 7.67 |
| `wells_dist100_model` | 1.01x | 0.0422 | 3.26 |
| `s2_wiener` | 1x | 0.0721 | 3.76 |
| `wells_daae_c_model` | 1x | 0.0519 | 3.45 |
| `hmm_drive_0` | 0.996x | 0.341 | 5.15 |
| `diamonds` | 0.996x | 0.0912 | 3.49 |
| `i320_pois_trunc_both` | 0.992x | 0.0751 | 3.6 |
| `cm_betadiscrete` | 0.991x | 3.89 | 8.21 |
| `iohmm_reg` | 0.991x | 0.908 | 6.92 |
| `wells_dae_model` | 0.987x | 0.0503 | 3.28 |
| `hmm_gaussian` | 0.986x | 0.733 | 5.43 |
| `i320_pois_trunc_ub` | 0.974x | 0.0603 | 3.51 |
| `sw_cumulative` | 0.953x | 0.0544 | 3.83 |
| `ch14_m14_11` | 0.9x | 1.62 | 5.85 |
| `i320_gp_matern32` | 0.898x | 0.0767 | 7.28 |
| `s2_ar_cov` | 0.869x | 0.0339 | 7.68 |
| `aalto_gpareto` | 0.841x | 0.0134 | 3.1 |
| `s2_logistic_normal` | 0.825x | 0.0671 | 6.75 |
| `s2_cosy` | 0.817x | 0.0322 | 6.81 |
| `garch11` | 0.812x | 0.0263 | 2.96 |
| `s2_fcor` | 0.803x | 0.0426 | 4.93 |
| `arma11` | 0.68x | 0.0198 | 2.99 |
| `s2_com_poisson` | 0.206x | 0.497 | 4.8 |
| `cm_invgaussian` | 0.0529x | 2.11 | 33.4 |

Each estimate is the engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli gradient | Stanli estimate (s) | CmdStan estimate (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2797) |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.000113 |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 5344) |
| `s2_mi_trunc_lb` | — | — | — | failed; density/gradient mismatch: scaled error 2.99e-07 |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 6059) |
| `sw_cens` | — | — | — | failed; density/gradient mismatch: scaled error 2.44e-06 |
<!--/gen-->
