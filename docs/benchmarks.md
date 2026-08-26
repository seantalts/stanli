# How much faster is stanli?

Across 119 posteriordb models, stanli evaluates a gradient **2.91x faster
than CmdStan at the median**. It is at least as fast on 116 of the 119 models.
Because stanli does not build a native C++ binary for each model, the first
complete run is typically faster by more than the gradient ratio alone
suggests.

## Eight Schools: 3.2x faster gradients, roughly 100x faster to draws

The non-centered Eight Schools model is a useful first result because it is
small: there is little work over which either engine can hide overhead.

| measurement | stanli | CmdStan | speedup |
| --- | ---: | ---: | ---: |
| one gradient at the same point | 234 ns | 745 ns | **3.18x** |
| first 1,000 warmup + 1,000 draw run | 0.03 s | 3.4 s | **roughly 100x** |

The stanli run is the whole command, from Stan source through model loading,
sampling, and CSV output. The CmdStan total is its 3.2 s model build plus its
0.20 s run. The source timings are recorded to only two or one decimal places,
so the headline and the first-run table columns deliberately use approximate
ratios.

The gradient row is the controlled comparison: both engines evaluate the same
sampling gradient at the same deterministic unconstrained point. The complete
run is what a user waits for, but it is indicative rather than controlled
because small numerical differences can send NUTS down different adaptation
and leapfrog trajectories.

## Representative models

Here is a deliberately mixed slice of the corpus, sorted from the largest
gradient win to the losses. It includes IRT, regression, hierarchical,
mixture, Gaussian-process, state-space, HMM, GARCH, and ODE models. Lower times
are better; higher speedups are better.

| model | stanli gradient | CmdStan gradient | gradient speedup | stanli source-to-CSV | CmdStan build + run | approx. first-run speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 121.131 us | 1.338 ms | 11.04x | 9.84 s | 166.9 s | ~17x |
| `dogs` | 7.130 us | 63.747 us | 8.94x | 0.56 s | 5.9 s | ~11x |
| `radon_pooled` | 45.378 us | 320.938 us | 7.07x | 0.59 s | 6.4 s | ~11x |
| `GLM_Poisson_model` | 386 ns | 2.008 us | 5.20x | 0.06 s | 3.4 s | ~57x |
| `state_space_stochastic_level_stochastic_seasonal` | 6.667 us | 26.320 us | 3.95x | 18.51 s | 44.4 s | ~2.4x |
| `eight_schools_noncentered` | 234 ns | 745 ns | 3.18x | 0.03 s | 3.4 s | ~110x |
| `logistic_regression_rhs` | 40.816 us | 113.106 us | 2.77x | 12.05 s | 20.8 s | ~1.7x |
| `normal_mixture` | 41.809 us | 88.239 us | 2.11x | 0.44 s | 3.6 s | ~8.2x |
| `hierarchical_gp` | 29.875 us | 47.565 us | 1.59x | 14.33 s | 25.8 s | ~1.8x |
| `hmm_example` | 17.462 us | 27.145 us | 1.55x | 0.51 s | 5.0 s | ~9.8x |
| `garch11` | 7.004 us | 9.664 us | 1.38x | 0.23 s | 3.2 s | ~14x |
| `diamonds` | 31.110 us | 31.497 us | 1.01x | 51.55 s | 51.9 s | ~1.0x |
| `one_comp_mm_elim_abs` | 522.915 us | 470.681 us | 0.90x | 10.55 s | 14.5 s | ~1.4x |
| `soil_incubation` | 67.783 us | 60.871 us | 0.90x | 11.94 s | 16.1 s | ~1.4x |
| `lotka_volterra` | 47.622 us | 41.313 us | 0.87x | 5.59 s | 10.3 s | ~1.8x |

Across all 117 models that completed a full run in both engines, the median
source-to-CSV speedup is **about 6.8x**, including CmdStan's model build. 115 of 117
finish at least as fast in stanli. As above, gradient speed is the controlled
result; full-run speed also reflects the trajectory taken by each sampler.
## What tends to win, and where it does not

**The largest wins are models with repeated independent work.** Regressions,
GLMs, IRT models, and many hierarchical models spend most of their time doing
the same operation for many observations. stanli executes those regions as a
few vector operations and reuses the resulting autodiff graph. CmdStan builds
and tears down scalar autodiff tape nodes on every gradient evaluation.

**Dense kernels and sequential models land closer to parity.** If most of a
gradient is one large matrix operation, both engines spend their time in the
same stan-math kernel. HMM, ARMA, and GARCH recurrences depend on the previous
step, so they cannot become independent vector lanes. stanli still wins on the
measured examples, but by less.

**The only gradient losses are three ODE models.** `one_comp_mm_elim_abs`,
`soil_incubation`, and `lotka_volterra` run at 0.87-0.90x CmdStan. Both engines
use the same Stan Math integrators; stanli still dispatches the compiled
right-hand side on each solver callback, while CmdStan has that model-specific
code inlined into its native binary. All three nevertheless finish the first
complete run faster once CmdStan's model build is included.

## Parallel chains

Chains run concurrently by default, with one executor and RNG stream per
chain. On an intentionally sequential 200-step ordered-logistic model, eight
chains scaled like this:

| worker threads | 1 | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| eight chains, wall time | 2.89 s | 1.59 s | 0.86 s | 0.49 s |

Parallelism does not change the draws: an eight-chain run is byte-identical to
the same chains run sequentially. This is checked across four models and
asserted in `tests/test_multichain.cpp` and `tests/test_python.py`.

## Numerical agreement

The performance results sit behind a differential oracle, not a separate
approximation. 118 of 120 posteriordb models are verified against CmdStan's
log density and complete gradient; 41 agree bitwise, and the worst relative
deviation is 2.6e-12. See [the per-model accuracy table](corpus-status.md) for
the two documented exceptions and every model's error bound.

## Full corpus

Every completed posteriordb model is below, in the same high-to-low gradient
order as the representative slice. `stanli source-to-CSV` times the complete
`stanli_run` process. `CmdStan build + run` adds the separately measured model
build and sampling command. Both runs use 1,000 warmup iterations, 1,000 draws,
and seed 1.

| model | stanli gradient | CmdStan gradient | gradient speedup | stanli source-to-CSV | CmdStan build | CmdStan build + run | approx. first-run speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 121.131 us | 1.338 ms | 11.04x | 9.84 s | 5.0 s | 166.9 s | ~17x |
| `grsm_latent_reg_irt` | 70.870 us | 762.133 us | 10.75x | 3.99 s | 4.9 s | 71.8 s | ~18x |
| `dogs` | 7.130 us | 63.747 us | 8.94x | 0.56 s | 3.5 s | 5.9 s | ~11x |
| `arK` | 1.748 us | 12.459 us | 7.13x | 0.16 s | 2.8 s | 3.8 s | ~24x |
| `radon_pooled` | 45.378 us | 320.938 us | 7.07x | 0.59 s | 2.6 s | 6.4 s | ~11x |
| `logmesquite_logvash` | 433 ns | 2.841 us | 6.56x | 0.10 s | 3.2 s | 3.6 s | ~36x |
| `logmesquite_logvas` | 486 ns | 3.130 us | 6.44x | 0.11 s | 3.2 s | 3.6 s | ~33x |
| `mesquite` | 478 ns | 3.055 us | 6.39x | 0.19 s | 2.9 s | 3.5 s | ~18x |
| `rats_model` | 1.067 us | 6.475 us | 6.07x | 0.16 s | 2.9 s | 3.4 s | ~21x |
| `logmesquite` | 484 ns | 2.902 us | 6.00x | 0.07 s | 3.0 s | 3.3 s | ~48x |
| `radon_hierarchical_intercept_centered` | 97.078 us | 569.143 us | 5.86x | 6.57 s | 3.2 s | 47.9 s | ~7.3x |
| `radon_hierarchical_intercept_noncentered` | 97.652 us | 570.300 us | 5.84x | 11.23 s | 3.3 s | 59.3 s | ~5.3x |
| `logmesquite_logva` | 358 ns | 2.070 us | 5.78x | 0.07 s | 3.0 s | 3.3 s | ~47x |
| `radon_county_intercept` | 81.602 us | 431.614 us | 5.29x | 5.77 s | 2.9 s | 30.3 s | ~5.2x |
| `radon_variable_intercept_noncentered` | 81.960 us | 430.721 us | 5.26x | 7.86 s | 3.1 s | 36.9 s | ~4.7x |
| `radon_variable_intercept_centered` | 81.803 us | 427.262 us | 5.22x | 4.98 s | 2.9 s | 25.9 s | ~5.2x |
| `GLM_Poisson_model` | 386 ns | 2.008 us | 5.20x | 0.06 s | 3.2 s | 3.4 s | ~57x |
| `dogs_log` | 8.133 us | 41.387 us | 5.09x | 0.47 s | 3.2 s | 4.2 s | ~9.0x |
| `radon_variable_slope_noncentered` | 83.421 us | 422.894 us | 5.07x | 10.56 s | 3.1 s | 55.0 s | ~5.2x |
| `radon_variable_slope_centered` | 83.519 us | 420.987 us | 5.04x | 5.12 s | 2.9 s | 26.6 s | ~5.2x |
| `logmesquite_logvolume` | 272 ns | 1.304 us | 4.79x | 0.03 s | 2.9 s | 3.1 s | ~100x |
| `Rate_2_model` | 118 ns | 561 ns | 4.75x | 0.02 s | 2.5 s | 2.7 s | ~140x |
| `kilpisjarvi` | 336 ns | 1.532 us | 4.56x | 0.82 s | 2.7 s | 4.3 s | ~5.2x |
| `Mt_model` | 4.466 us | 19.984 us | 4.47x | 0.08 s | 3.4 s | 3.9 s | ~48x |
| `nes` | 16.100 us | 69.324 us | 4.31x | 1.87 s | 3.1 s | 9.8 s | ~5.2x |
| `kidscore_interaction_c` | 2.489 us | 10.333 us | 4.15x | 0.09 s | 2.9 s | 3.3 s | ~37x |
| `Rate_1_model` | 63 ns | 260 ns | 4.13x | 0.02 s | 2.3 s | 2.4 s | ~120x |
| `radon_partially_pooled_centered` | 66.311 us | 272.243 us | 4.11x | 3.81 s | 2.9 s | 16.9 s | ~4.4x |
| `radon_partially_pooled_noncentered` | 67.072 us | 273.685 us | 4.08x | 5.24 s | 3.1 s | 23.2 s | ~4.4x |
| `Mth_model` | 23.405 us | 93.922 us | 4.01x | 2.41 s | 4.2 s | 9.6 s | ~4.0x |
| `kidscore_interaction_c2` | 2.487 us | 9.901 us | 3.98x | 0.11 s | 2.8 s | 3.2 s | ~29x |
| `kidscore_interaction_z` | 2.518 us | 10.013 us | 3.98x | 0.12 s | 2.9 s | 3.4 s | ~28x |
| `kidscore_mom_work` | 2.505 us | 9.959 us | 3.98x | 0.14 s | 2.8 s | 3.4 s | ~25x |
| `state_space_stochastic_level_stochastic_seasonal` | 6.667 us | 26.320 us | 3.95x | 18.51 s | 4.6 s | 44.4 s | ~2.4x |
| `sesame_one_pred_a` | 875 ns | 3.440 us | 3.93x | 0.04 s | 2.7 s | 2.9 s | ~73x |
| `kidscore_interaction` | 2.555 us | 9.927 us | 3.89x | 0.69 s | 2.9 s | 4.8 s | ~7.0x |
| `radon_variable_intercept_slope_noncentered` | 120.376 us | 441.463 us | 3.67x | 17.51 s | 3.3 s | 61.9 s | ~3.5x |
| `radon_variable_intercept_slope_centered` | 119.490 us | 437.889 us | 3.66x | 9.52 s | 3.1 s | 30.3 s | ~3.2x |
| `seeds_centered_model` | 737 ns | 2.650 us | 3.60x | 0.08 s | 3.7 s | 4.0 s | ~50x |
| `M0_model` | 4.394 us | 15.595 us | 3.55x | 0.06 s | 2.7 s | 3.1 s | ~51x |
| `logearn_interaction_z` | 7.465 us | 26.484 us | 3.55x | 0.24 s | 3.0 s | 3.8 s | ~16x |
| `election88_full` | 256.087 us | 901.961 us | 3.52x | 123.34 s | 3.9 s | 472.0 s | ~3.8x |
| `kidscore_momhsiq` | 2.032 us | 7.145 us | 3.52x | 0.29 s | 2.8 s | 3.6 s | ~13x |
| `GLMM_Poisson_model` | 696 ns | 2.412 us | 3.47x | 0.16 s | 3.7 s | 4.4 s | ~27x |
| `logearn_interaction` | 7.536 us | 26.051 us | 3.46x | 3.09 s | 2.8 s | 11.3 s | ~3.7x |
| `GLMM1_model` | 10.394 us | 35.558 us | 3.42x | 1.06 s | 3.2 s | 4.9 s | ~4.6x |
| `dogs_hierarchical` | 10.251 us | 34.053 us | 3.32x | 0.23 s | 2.7 s | 3.4 s | ~15x |
| `Mtbh_model` | 12.895 us | 42.791 us | 3.32x | 1.08 s | 4.9 s | 7.3 s | ~6.7x |
| `Rate_4_model` | 94 ns | 311 ns | 3.31x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `seeds_stanified_model` | 714 ns | 2.341 us | 3.28x | 0.08 s | 3.4 s | 3.7 s | ~46x |
| `blr` | 536 ns | 1.728 us | 3.22x | 0.05 s | 3.1 s | 3.3 s | ~66x |
| `logearn_height_male` | 5.949 us | 19.147 us | 3.22x | 1.55 s | 2.8 s | 6.6 s | ~4.2x |
| `Rate_5_model` | 82 ns | 262 ns | 3.20x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `surgical_model` | 528 ns | 1.684 us | 3.19x | 0.05 s | 3.3 s | 3.5 s | ~71x |
| `eight_schools_noncentered` | 234 ns | 745 ns | 3.18x | 0.03 s | 3.2 s | 3.4 s | ~110x |
| `logearn_logheight_male` | 5.882 us | 18.697 us | 3.18x | 4.88 s | 2.8 s | 16.2 s | ~3.3x |
| `Rate_3_model` | 85 ns | 268 ns | 3.15x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `dugongs_model` | 529 ns | 1.653 us | 3.12x | 0.05 s | 3.0 s | 3.2 s | ~65x |
| `kidscore_momiq` | 1.576 us | 4.861 us | 3.08x | 0.16 s | 2.7 s | 3.1 s | ~20x |
| `seeds_model` | 733 ns | 2.130 us | 2.91x | 0.09 s | 3.5 s | 3.8 s | ~42x |
| `kidscore_momhs` | 1.555 us | 4.483 us | 2.88x | 0.07 s | 2.7 s | 3.0 s | ~43x |
| `bym2_offset_only` | 39.938 us | 114.620 us | 2.87x | 16.70 s | 4.1 s | 27.5 s | ~1.6x |
| `logistic_regression_rhs` | 40.816 us | 113.106 us | 2.77x | 12.05 s | 4.6 s | 20.8 s | ~1.7x |
| `log10earn_height` | 4.200 us | 11.560 us | 2.75x | 0.98 s | 2.7 s | 4.5 s | ~4.5x |
| `multi_occupancy` | 22.267 us | 58.996 us | 2.65x | 2.48 s | 5.7 s | 13.0 s | ~5.3x |
| `earn_height` | 4.143 us | 10.866 us | 2.62x | 0.81 s | 2.7 s | 4.6 s | ~5.6x |
| `Mh_model` | 15.184 us | 38.956 us | 2.57x | 1.42 s | 3.2 s | 5.9 s | ~4.1x |
| `pilots` | 734 ns | 1.878 us | 2.56x | 0.66 s | 3.2 s | 4.5 s | ~6.8x |
| `logearn_height` | 4.427 us | 11.162 us | 2.52x | 0.85 s | 2.7 s | 4.4 s | ~5.2x |
| `losscurve_sislob` | 1.378 us | 3.450 us | 2.50x | 0.17 s | 4.1 s | 4.4 s | ~26x |
| `lsat_model` | 37.934 us | 91.173 us | 2.40x | 2.59 s | 3.6 s | 8.3 s | ~3.2x |
| `irt_2pl` | 16.456 us | 37.468 us | 2.28x | 1.32 s | 3.9 s | 6.1 s | ~4.6x |
| `GLM_Binomial_model` | 832 ns | 1.809 us | 2.17x | 0.06 s | 3.2 s | 3.4 s | ~57x |
| `ldaK2` | 48.159 us | 104.059 us | 2.16x | 1.29 s | 3.6 s | 6.8 s | ~5.3x |
| `normal_mixture` | 41.809 us | 88.239 us | 2.11x | 0.44 s | 2.5 s | 3.6 s | ~8.2x |
| `dogs_nonhierarchical` | 19.442 us | 40.588 us | 2.09x | 1.13 s | 6.8 s | 9.7 s | ~8.5x |
| `low_dim_gauss_mix_collapse` | 46.143 us | 95.373 us | 2.07x | 2.21 s | 3.0 s | 7.5 s | ~3.4x |
| `low_dim_gauss_mix` | 48.184 us | 98.315 us | 2.04x | 0.82 s | 3.0 s | 5.0 s | ~6.1x |
| `wells_dist` | 21.031 us | 39.202 us | 1.86x | 0.68 s | 2.8 s | 4.2 s | ~6.2x |
| `normal_mixture_k` | 192.792 us | 357.439 us | 1.85x | 60.79 s | 3.3 s | 104.9 s | ~1.7x |
| `2pl_latent_reg_irt` | 82.828 us | 134.556 us | 1.62x | 6.43 s | 5.3 s | 13.2 s | ~2.1x |
| `hierarchical_gp` | 29.875 us | 47.565 us | 1.59x | 14.33 s | 8.6 s | 25.8 s | ~1.8x |
| `hmm_example` | 17.462 us | 27.145 us | 1.55x | 0.51 s | 4.0 s | 5.0 s | ~9.8x |
| `covid19imperial_v2` | 237.159 us | 345.937 us | 1.46x | 105.88 s | 6.7 s | 182.7 s | ~1.7x |
| `covid19imperial_v3` | 235.122 us | 342.943 us | 1.46x | 105.25 s | 6.7 s | 182.4 s | ~1.7x |
| `accel_splines` | 7.333 us | 10.584 us | 1.44x | 16.40 s | 4.3 s | 24.0 s | ~1.5x |
| `hmm_gaussian` | 184.527 us | 263.917 us | 1.43x | 276.77 s | 4.6 s | 23.4 s | ~0.08x |
| `hier_2pl` | 279.152 us | 397.603 us | 1.42x | 17.37 s | 6.5 s | 33.3 s | ~1.9x |
| `accel_gp` | 6.700 us | 9.532 us | 1.42x | 12.77 s | 5.3 s | 22.3 s | ~1.7x |
| `iohmm_reg` | 227.247 us | 320.335 us | 1.41x | 107.91 s | 5.7 s | 186.9 s | ~1.7x |
| `garch11` | 7.004 us | 9.664 us | 1.38x | 0.23 s | 2.8 s | 3.2 s | ~14x |
| `arma11` | 4.494 us | 6.158 us | 1.37x | 0.11 s | 2.9 s | 3.2 s | ~29x |
| `prophet` | 54.585 us | 69.789 us | 1.28x | 92.72 s | 4.7 s | 122.4 s | ~1.3x |
| `hmm_drive_1` | 116.620 us | 147.829 us | 1.27x | 4.43 s | 4.7 s | 11.6 s | ~2.6x |
| `nes_logit_model` | 6.119 us | 7.653 us | 1.25x | 0.15 s | 3.0 s | 3.4 s | ~23x |
| `eight_schools_centered` | 256 ns | 314 ns | 1.23x | 0.04 s | 2.9 s | 3.1 s | ~77x |
| `hmm_drive_0` | 110.835 us | 132.850 us | 1.20x | 2.99 s | 4.5 s | 8.2 s | ~2.7x |
| `Mb_model` | 41.691 us | 49.570 us | 1.19x | 1.06 s | 3.2 s | 4.3 s | ~4.1x |
| `kronecker_gp` | 183.343 us | 217.990 us | 1.19x | 369.81 s | 8.0 s | 459.1 s | ~1.2x |
| `gp_regr` | 4.040 us | 4.698 us | 1.16x | 0.07 s | 5.2 s | 5.4 s | ~78x |
| `Survey_model` | 53.752 us | 61.578 us | 1.15x | 1.12 s | 2.9 s | 4.0 s | ~3.6x |
| `bones_model` | 45.786 us | 51.501 us | 1.12x | 0.76 s | 3.4 s | 4.7 s | ~6.2x |
| `radon_county` | 73.628 us | 82.076 us | 1.11x | 4.04 s | 3.0 s | 7.5 s | ~1.9x |
| `wells_dist100_model` | 15.526 us | 17.195 us | 1.11x | 0.25 s | 3.0 s | 3.5 s | ~14x |
| `wells_dist100ars_model` | 17.171 us | 18.997 us | 1.11x | 0.40 s | 3.0 s | 3.6 s | ~9.0x |
| `wells_dae_inter_model` | 19.619 us | 21.310 us | 1.09x | 0.31 s | 3.2 s | 3.8 s | ~12x |
| `wells_dae_c_model` | 17.808 us | 19.308 us | 1.08x | 0.38 s | 3.2 s | 3.8 s | ~10.0x |
| `wells_dae_model` | 18.820 us | 20.356 us | 1.08x | 0.55 s | 3.1 s | 3.9 s | ~7.0x |
| `wells_interaction_model` | 18.880 us | 20.402 us | 1.08x | 0.69 s | 3.1 s | 4.0 s | ~5.9x |
| `nn_rbm1bJ10` | 172.408 us | 185.731 us | 1.08x | 490.01 s | 5.2 s | 462.0 s | ~0.94x |
| `wells_daae_c_model` | 19.439 us | 20.885 us | 1.07x | 0.50 s | 3.2 s | 3.8 s | ~7.7x |
| `wells_interaction_c_model` | 18.924 us | 20.272 us | 1.07x | 0.26 s | 3.1 s | 3.6 s | ~14x |
| `gp_pois_regr` | 3.785 us | 3.935 us | 1.04x | 1.30 s | 5.4 s | 6.9 s | ~5.3x |
| `diamonds` | 31.110 us | 31.497 us | 1.01x | 51.55 s | 3.4 s | 51.9 s | ~1.0x |
| `one_comp_mm_elim_abs` | 522.915 us | 470.681 us | 0.90x | 10.55 s | 3.3 s | 14.5 s | ~1.4x |
| `soil_incubation` | 67.783 us | 60.871 us | 0.90x | 11.94 s | 3.3 s | 16.1 s | ~1.4x |
| `lotka_volterra` | 47.622 us | 41.313 us | 0.87x | 5.59 s | 4.1 s | 10.3 s | ~1.8x |

120 models; 119 with both gradients; median per-gradient speedup 2.91x; 116/119
at or above CmdStan. 117 completed first runs; median source-to-CSV speedup
about 6.8x; 115/117 at or above CmdStan including its model build.

The extreme `hmm_gaussian` first-run result is not a useful speed comparison:
every post-warmup draw in CmdStan's retained seed-1 run was divergent, so the
two engines did radically different sampling work. Its controlled gradient
row, 1.43x in stanli's favor, is the meaningful result.

### Runs that did not complete

A missing run time is not a missing gradient. These rows sort below the
complete table, and their measured gradient ratios still stand.

| model | stanli gradient | CmdStan gradient | gradient speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 2.360 ms | 5.580 ms | 2.36x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `nn_rbm1bJ100` | 411.222 ms | 434.981 ms | 1.06x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `sir` | - | - | - | stanli's gradient probe threw at the benchmark point; no stanli gradient |

`sir` has no gradient number because the fixed probe point makes its ODE
solution dip to -4.4e-10 and `poisson_lpmf` rejects the negative rate. The
model itself samples successfully. `ldaK5` and `nn_rbm1bJ100` completed both
gradient probes before both sampling commands reached the 900 s cap.

## Benchmark method

Measured 2026-08-25 on an Apple M3 Ultra (macOS arm64) with Apple clang 21,
single-threaded, with both engines built at `-O3` and
`-ffp-contract=off`. The stanli columns are one refreshed 120-model run. The
CmdStan columns carry over from a 2026-08-06 run on the same host because no
stanli change can affect them. CmdStan's one-time `make build` setup was
already complete, so its per-model build column uses the warm precompiled
header path and does not include that earlier setup cost.

For gradients, both engines evaluate the sampling log density (proportional
terms plus Jacobian) at the same deterministic unconstrained point. stanli
runs `tools/bench_grad.cpp`; CmdStan runs `tools/bench_cmdstan_grad.cpp` over
the stanc-generated model. Both loop the same fresh-vars, gradient, and memory
recovery cycle that a leapfrog step performs. The reported cells are warmed
arithmetic means from one timed loop per model.

For complete runs, `stanli_sample_s` in
[`corpus-bench.tsv`](corpus-bench.tsv) measures the entire `stanli_run`
process from Stan source to CSV. CmdStan's build and execution are timed
separately, so the displayed total adds `cmdstan_build_s` and
`cmdstan_sample_s`. The sampler rows are real wall-clock observations, not a
fixed-work microbenchmark; use the gradient rows when comparing engine
throughput independent of a particular NUTS trajectory.

## For developers

The implementation story is intentionally elsewhere. For a conceptual
overview, read [How stanli works](how-it-works.md). For graph re-rolling, lane
partitioning, generated adjoints, tape islands, the compiled ODE right-hand
side, and their targeted A/B measurements, read
[Graph optimizations and performance work](../runtime/src/OPTIMIZATIONS.md).

## Reproducing

```sh
./tools/dev_setup.sh --corpus          # deps, build, posteriordb, CmdStan
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  docs/corpus-bench.tsv --stanli-only --timeout 900
# To remeasure both sides, omit --stanli-only and use a new output TSV.
python3 tools/corpus_table.py docs/corpus-bench.tsv
python3 harnesses/ab_corpus.py deps/posteriordb
```
