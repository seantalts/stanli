# Performance measurements

The benchmark runner uses the same model inventory as numerical testing.
Application models from every source collection participate in the default
`--corpus all` run; language-conformance fixtures remain numerical tests.
Collection names identify provenance and can be used as optional filters.
The [corpus inventory](../../docs/corpus-status.md) describes numerical coverage, and the
[benchmark protocol](../../docs/benchmarks.md#how-we-measure) defines each timed boundary.

| Measurement | What it answers | Timed work |
| --- | --- | --- |
| [Warm gradients](#warm-gradients) | How fast is repeated density and gradient evaluation? | Evaluation at a shared fixed point, after equal warmup windows. |
| [Complete sampling](#complete-sampling) | How long does a fixed inference budget take? | CLI startup, Stanli preparation, adaptation, sampling, generated quantities and CSV output; CmdStan uses an already-built model. |
| [Compilation and preparation](#compilation-and-preparation) | What does the first fit cost? | Report source compilation, preparation from existing MIR and complete execution separately. |

The measurements below retain their original dates, revisions and protocols.
No complete sweep of the newly unified inventory has been run. Results from
different runs are not pooled into a new aggregate.

## Warm gradients

The current protocol measures six alternating pairs, checks the full gradient
before accepting each pair, and retains medians and median absolute deviations
(MAD). The following mixed examples come from the **16 September 2026** run
`fd5e0ecacddc7047`, measured at runtime/compiler revision `6e462c2e` on an Apple
M3 Ultra, macOS ARM64. Each row's speedup is the median of its within-pair ratios.

| model | source | Stanli gradient | CmdStan gradient | paired speedup |
| --- | --- | ---: | ---: | ---: |
| `aalto_gpareto` | Aalto | 0.386 us | 0.261 us | 0.69x ± 0.02 MAD |
| `ch09_m9_1` | Rethinking | 1.020 us | 2.467 us | 2.41x ± 0.07 MAD |
| `sw_gaussian` | brms | 0.358 us | 0.435 us | 1.21x ± 0.04 MAD |
| `aalto_poisson_hurdle` | Aalto | 2.188 us | 58.565 us | 26.96x ± 0.80 MAD |
| `ch12_m12_4` | Rethinking | 24.921 us | 2813.958 us | 111.93x ± 2.99 MAD |
| `s2_gev` | brms | 13.829 us | 4.994 us | 0.37x ± 0.01 MAD |

[Raw paired summaries](../../output/teaching-performance/benchmark-summary.tsv),
[run identity](../../output/teaching-performance/benchmark-manifest.json), and the
[full appendix](../../output/teaching-performance/README.md) retain all 199 fixtures,
including failures. These examples do not define a corpus-wide speedup.

## Complete sampling

The same September 16 run used four single-chain seeds per engine, with
1,000 warmup iterations and 1,000 retained draws per seed. Each CLI duration
includes generated quantities and CSV output. CmdStan compilation was timed
separately. This is fixed-budget runtime, not time to equal inferential accuracy.

| model | source | Stanli CLI median [range], seconds | CmdStan CLI median [range], seconds | CmdStan/Stanli | diagnostic screen, Stanli/CmdStan |
| --- | --- | ---: | ---: | ---: | --- |
| `aalto_gpareto` | Aalto | 0.027 [0.025–0.029] | 0.025 [0.022–0.228] | 0.939x | clear/clear |
| `ch09_m9_1` | Rethinking | 0.029 [0.026–0.032] | 0.053 [0.046–0.252] | 1.82x | clear/clear |
| `sw_gaussian` | brms | 0.020 [0.019–0.021] | 0.029 [0.022–0.201] | 1.49x | clear/clear |
| `aalto_poisson_hurdle` | Aalto | 0.600 [0.589–0.602] | 0.827 [0.783–1.05] | 1.38x | clear/clear |
| `ch12_m12_4` | Rethinking | 0.586 [0.558–0.600] | 86.06 [80.32–89.47] | 146.9x | clear/clear |
| `s2_gev` | brms | 0.225 [0.219–0.247] | 0.095 [0.090–0.292] | 0.420x | clear/clear |
| `sw_re_negbin` | brms | capped 1/4 seeds | 5.01 [0.216–5.84] | — | incomplete/review |

Of 199 fixtures, 193 completed in both engines and 187 had lower Stanli CLI
medians. The experiment's practical target was CmdStan/Stanli ≥ 0.8:
189 met it, four fell below it, and six lacked a complete comparison.
Failed or capped seeds prevent an aggregate time; diagnostic flags remain
visible. The [appendix](../../output/teaching-performance/README.md) defines the
diagnostic screen and retains every model. Later fixes and their
[separate measurements](2026-09-16-brms-performance.md) do not change this frozen run.

### Parallel chains

Chains run concurrently by default, with one executor and RNG stream per
chain. On an intentionally sequential 200-step ordered-logistic model, eight
chains scaled like this:

| worker threads | 1 | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| eight chains, wall time | 2.89 s | 1.59 s | 0.86 s | 0.49 s |

Parallelism does not change the draws: an eight-chain run is byte-identical to
the same chains run sequentially. This is checked across four models and
asserted in `tests/test_multichain.cpp` and `tests/test_python.py`.

## Compilation and preparation

Preparation from existing MIR excludes source compilation and first evaluation.
A first-fit estimate that adds measured CmdStan compilation to a CLI duration
must be labeled as a sum of stages. It is not a directly timed cold-cache fit.
The [September 16 appendix](../../output/teaching-performance/README.md) reports
these stages per model; a [separate fresh-R-session measurement](../../docs/teaching.md#time-from-a-fresh-r-session-to-the-first-posterior)
includes package startup and the first fit.

The following example is from the September 11 posteriordb run described in
[Benchmark method](#benchmark-method).

### Eight Schools: 1.6x faster gradients, roughly 100x faster to draws

The non-centered Eight Schools model is a useful first result because it is
small: there is little work over which either engine can hide overhead.

| measurement | stanli | CmdStan | speedup |
| --- | ---: | ---: | ---: |
| one gradient at the same point | 215 ns | 350 ns | **1.63x** |
| first 1,000 warmup + 1,000 draw run | 0.03 s | 3.4 s | **roughly 100x** |

The stanli run is the whole command, from Stan source through model loading,
sampling, and CSV output. The CmdStan total is its 3.1 s model build plus its
0.27 s run. The source timings are recorded to only two or one decimal places,
so the headline and the first-run table columns deliberately use approximate
ratios.

The historical gradient row compares the same fixed input: both engines evaluate the same
sampling gradient at the same deterministic unconstrained point. The complete
run is what a user waits for, but it is indicative rather than controlled
because small numerical differences can send NUTS down different adaptation
and leapfrog trajectories.

## Historical posteriordb measurements

Across 119 posteriordb models in the September 11 measurements, stanli evaluates
a gradient **2.10x faster than CmdStan at the median**. It is at least as fast
on 119 of the 119 measured models. The tables below retain that run's values;
they predate the paired protocol and have no repeated-trial uncertainty estimates.
They describe the posteriordb subset, not the complete current inventory.
The [historical experiments archive](benchmark-history.md) retains additional
September 14 sampling and gradient measurements with their own protocols.

## Representative models

Here is a deliberately mixed slice of that historical posteriordb run, spanning the range of measured
gradient speedups. It includes IRT, regression, hierarchical,
mixture, Gaussian-process, state-space, HMM, GARCH, and ODE models. Lower times
are better; higher speedups are better.

| model | stanli gradient | CmdStan gradient | gradient speedup | stanli source-to-CSV | CmdStan build + run | approx. first-run speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 122.589 us | 1.338 ms | 10.91x | 9.45 s | 166.3 s | ~18x |
| `dogs` | 6.180 us | 62.233 us | 10.07x | 0.48 s | 5.9 s | ~12x |
| `radon_pooled` | 43.745 us | 321.613 us | 7.35x | 0.51 s | 6.4 s | ~13x |
| `GLM_Poisson_model` | 358 ns | 992 ns | 2.77x | 0.06 s | 3.4 s | ~57x |
| `state_space_stochastic_level_stochastic_seasonal` | 6.418 us | 19.648 us | 3.06x | 10.02 s | 44.9 s | ~4.5x |
| `eight_schools_noncentered` | 215 ns | 350 ns | 1.63x | 0.03 s | 3.4 s | ~110x |
| `logistic_regression_rhs` | 39.739 us | 96.549 us | 2.43x | 9.98 s | 21.2 s | ~2.1x |
| `soil_incubation` | 30.783 us | 58.876 us | 1.91x | 6.30 s | 16.3 s | ~2.6x |
| `normal_mixture` | 42.438 us | 87.584 us | 2.06x | 0.43 s | 3.7 s | ~8.5x |
| `lotka_volterra` | 21.627 us | 40.781 us | 1.89x | 2.19 s | 10.5 s | ~4.8x |
| `hmm_example` | 16.074 us | 26.172 us | 1.63x | 0.48 s | 4.9 s | ~10x |
| `garch11` | 6.900 us | 7.938 us | 1.15x | 0.19 s | 3.3 s | ~17x |
| `hierarchical_gp` | 19.566 us | 41.136 us | 2.10x | 13.86 s | 26.1 s | ~1.9x |
| `one_comp_mm_elim_abs` | 459.111 us | 462.810 us | 1.01x | 9.27 s | 14.5 s | ~1.6x |
| `diamonds` | 31.069 us | 31.897 us | 1.03x | 49.65 s | 52.4 s | ~1.1x |
| `gp_regr` | 2.669 us | 3.256 us | 1.22x | 0.51 s | 5.5 s | ~11x |
| `gp_pois_regr` | 2.274 us | 2.667 us | 1.17x | 0.77 s | 7.1 s | ~9.3x |

Across all 117 models that completed a full run in both engines, the median
source-to-CSV speedup is **about 8.6x**, including CmdStan's model build. 116 of 117
finish at least as fast in stanli. As above, gradient speed is the controlled
result; full-run speed also reflects the trajectory taken by each sampler.

## What tends to win, and where it does not

**The largest wins are models with repeated independent work.** Regressions,
GLMs, IRT models, and many hierarchical models spend most of their time doing
the same operation for many observations. stanli executes those regions as a
few vector operations and reuses the resulting autodiff graph. CmdStan builds
and tears down scalar autodiff tape nodes on every gradient evaluation.

**Dense kernels and sequential models land closer to parity.** If most of a
gradient is one large matrix operation, both engines compute its value in
the same stan-math kernel; where they differ is the adjoint, described
below. HMM, ARMA, and GARCH recurrences depend on the previous step, so
they cannot become independent vector lanes. stanli still wins on the
measured examples, but by less.

**The ODE models moved from stanli's weakest results to some of its
strongest.** `one_comp_mm_elim_abs`, `soil_incubation`, and `lotka_volterra`
ran at 0.87-0.90x CmdStan in the 0.10.0 benchmarks; they are now 1.01-1.91x.
All three call the legacy `integrate_ode_bdf`/`integrate_ode_rk45` interface,
and the change coincided with 0.11.0's expanded function coverage inside ODE
right-hand sides (see the changelog). Both engines still use the same Stan
Math integrator.

**The Gaussian-process models now use native covariance and Cholesky
pullbacks.** `gp_regr` and `gp_pois_regr`, formerly the two gradient losses,
now run at 1.22x and 1.17x the recorded CmdStan gradient throughput.
`hierarchical_gp` runs at 2.10x. Fixed-coordinate exponentiated-quadratic
covariances reuse their forward output to compute parameter derivatives;
Cholesky reuses its saved factor and Stan Math's pullback. This removes
nested-tape replay from these backward passes. Active coordinates, other
covariance families, and unsafe numerical cases retain the existing GP
fallback. The [implementation report](../../docs/superpowers/plans/2026-09-11-shared-data-native-pullbacks.md)
records matched before/after measurements and numerical checks; GP gradients
can differ by rounding and are not claimed to be bitwise identical.


## Numerical agreement

The shared [corpus replay](../../TESTING.md#comparison-with-cmdstan-on-complete-models)
compares recorded CmdStan log densities, every gradient component and available
constrained/generated outputs at three deterministic points. It preserves
matching domain refusals and documented ill-conditioned exceptions. See the
[complete inventory and numerical results](../../docs/corpus-status.md) for coverage.
Benchmark timing pairs also have their own fixed-point numerical gate.

<a id="full-corpus"></a>

## Historical posteriordb model results

Every completed posteriordb model is below, in the same high-to-low gradient
order as the representative slice. `stanli source-to-CSV` times the complete
`stanli_run` process. `CmdStan build + run` adds the separately measured model
build and sampling command. Both runs use 1,000 warmup iterations, 1,000 draws,
and seed 1.

| model | stanli gradient | CmdStan gradient | gradient speedup | stanli source-to-CSV | CmdStan build | CmdStan build + run | approx. first-run speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 122.589 us | 1.338 ms | 10.91x | 9.45 s | 5.1 s | 166.3 s | ~18x |
| `grsm_latent_reg_irt` | 71.404 us | 747.838 us | 10.47x | 4.84 s | 4.9 s | 71.7 s | ~15x |
| `dogs` | 6.180 us | 62.233 us | 10.07x | 0.48 s | 3.5 s | 5.9 s | ~12x |
| `radon_hierarchical_intercept_noncentered` | 65.807 us | 569.958 us | 8.66x | 7.56 s | 3.3 s | 59.3 s | ~7.8x |
| `radon_variable_intercept_noncentered` | 50.234 us | 432.185 us | 8.60x | 4.61 s | 3.1 s | 36.8 s | ~8.0x |
| `radon_hierarchical_intercept_centered` | 65.618 us | 563.946 us | 8.59x | 5.67 s | 3.1 s | 46.3 s | ~8.2x |
| `radon_variable_intercept_centered` | 49.766 us | 426.043 us | 8.56x | 3.01 s | 2.9 s | 26.0 s | ~8.6x |
| `radon_county_intercept` | 49.555 us | 423.960 us | 8.56x | 3.56 s | 2.8 s | 29.8 s | ~8.4x |
| `radon_variable_slope_centered` | 51.313 us | 423.835 us | 8.26x | 3.11 s | 2.9 s | 26.4 s | ~8.5x |
| `radon_variable_slope_noncentered` | 51.731 us | 425.500 us | 8.23x | 6.58 s | 3.1 s | 52.0 s | ~7.9x |
| `radon_partially_pooled_centered` | 34.599 us | 275.255 us | 7.96x | 2.07 s | 2.9 s | 17.1 s | ~8.3x |
| `radon_partially_pooled_noncentered` | 34.570 us | 272.083 us | 7.87x | 2.81 s | 3.1 s | 27.2 s | ~9.7x |
| `radon_variable_intercept_slope_noncentered` | 58.610 us | 444.145 us | 7.58x | 8.48 s | 3.3 s | 61.6 s | ~7.3x |
| `radon_variable_intercept_slope_centered` | 57.602 us | 434.004 us | 7.53x | 4.57 s | 3.1 s | 29.1 s | ~6.4x |
| `radon_pooled` | 43.745 us | 321.613 us | 7.35x | 0.51 s | 2.6 s | 6.4 s | ~13x |
| `arK` | 1.670 us | 11.357 us | 6.80x | 0.13 s | 2.7 s | 3.7 s | ~28x |
| `dogs_log` | 6.097 us | 39.998 us | 6.56x | 0.42 s | 3.1 s | 4.1 s | ~9.9x |
| `rats_model` | 1.026 us | 4.974 us | 4.85x | 0.14 s | 3.0 s | 3.5 s | ~25x |
| `nes` | 15.928 us | 68.929 us | 4.33x | 1.49 s | 3.1 s | 9.8 s | ~6.6x |
| `Mt_model` | 4.490 us | 19.042 us | 4.24x | 0.08 s | 3.3 s | 3.8 s | ~47x |
| `election88_full` | 215.507 us | 897.020 us | 4.16x | 104.72 s | 4.0 s | 481.5 s | ~4.6x |
| `mesquite` | 467 ns | 1.893 us | 4.05x | 0.14 s | 2.9 s | 3.5 s | ~25x |
| `logmesquite_logvas` | 460 ns | 1.804 us | 3.92x | 0.09 s | 3.2 s | 3.7 s | ~41x |
| `logmesquite` | 463 ns | 1.779 us | 3.84x | 0.06 s | 3.0 s | 3.4 s | ~57x |
| `Mth_model` | 23.607 us | 90.576 us | 3.84x | 2.36 s | 4.1 s | 9.5 s | ~4.0x |
| `kidscore_interaction_c` | 2.434 us | 8.802 us | 3.62x | 0.07 s | 2.9 s | 3.3 s | ~48x |
| `logmesquite_logvash` | 429 ns | 1.542 us | 3.59x | 0.09 s | 3.2 s | 3.7 s | ~41x |
| `kidscore_interaction_c2` | 2.454 us | 8.730 us | 3.56x | 0.08 s | 2.9 s | 3.4 s | ~42x |
| `kidscore_mom_work` | 2.460 us | 8.691 us | 3.53x | 0.12 s | 2.8 s | 3.4 s | ~29x |
| `kidscore_interaction_z` | 2.456 us | 8.621 us | 3.51x | 0.10 s | 2.9 s | 3.4 s | ~34x |
| `kidscore_interaction` | 2.486 us | 8.596 us | 3.46x | 0.50 s | 2.9 s | 4.8 s | ~9.7x |
| `logearn_interaction` | 7.215 us | 24.904 us | 3.45x | 2.56 s | 2.9 s | 11.4 s | ~4.5x |
| `logearn_interaction_z` | 7.369 us | 24.880 us | 3.38x | 0.22 s | 3.0 s | 3.9 s | ~18x |
| `M0_model` | 4.423 us | 14.696 us | 3.32x | 0.07 s | 2.6 s | 3.0 s | ~43x |
| `logearn_logheight_male` | 5.529 us | 17.589 us | 3.18x | 4.19 s | 2.8 s | 16.0 s | ~3.8x |
| `dogs_hierarchical` | 10.336 us | 32.744 us | 3.17x | 0.24 s | 2.7 s | 3.4 s | ~14x |
| `Mtbh_model` | 13.232 us | 41.824 us | 3.16x | 1.02 s | 4.8 s | 7.1 s | ~7.0x |
| `logearn_height_male` | 5.648 us | 17.669 us | 3.13x | 1.19 s | 2.8 s | 6.5 s | ~5.5x |
| `GLMM1_model` | 9.953 us | 30.838 us | 3.10x | 1.01 s | 3.1 s | 4.8 s | ~4.8x |
| `state_space_stochastic_level_stochastic_seasonal` | 6.418 us | 19.648 us | 3.06x | 10.02 s | 4.6 s | 44.9 s | ~4.5x |
| `logmesquite_logva` | 332 ns | 1.012 us | 3.05x | 0.06 s | 3.0 s | 3.3 s | ~56x |
| `kidscore_momhsiq` | 2.006 us | 5.928 us | 2.96x | 0.24 s | 2.8 s | 3.6 s | ~15x |
| `bym2_offset_only` | 35.571 us | 101.226 us | 2.85x | 12.85 s | 4.0 s | 27.4 s | ~2.1x |
| `GLM_Poisson_model` | 358 ns | 992 ns | 2.77x | 0.06 s | 3.1 s | 3.4 s | ~57x |
| `dogs_nonhierarchical` | 15.288 us | 40.321 us | 2.64x | 0.92 s | 6.6 s | 9.5 s | ~10x |
| `multi_occupancy` | 22.959 us | 58.221 us | 2.54x | 2.87 s | 5.7 s | 13.1 s | ~4.6x |
| `earn_height` | 4.044 us | 10.177 us | 2.52x | 0.71 s | 2.7 s | 4.6 s | ~6.5x |
| `log10earn_height` | 4.029 us | 10.053 us | 2.50x | 0.85 s | 2.7 s | 4.5 s | ~5.3x |
| `logearn_height` | 4.005 us | 9.965 us | 2.49x | 0.65 s | 2.7 s | 4.4 s | ~6.8x |
| `GLMM_Poisson_model` | 640 ns | 1.578 us | 2.47x | 0.17 s | 3.6 s | 4.3 s | ~25x |
| `logistic_regression_rhs` | 39.739 us | 96.549 us | 2.43x | 9.98 s | 4.7 s | 21.2 s | ~2.1x |
| `sesame_one_pred_a` | 820 ns | 1.926 us | 2.35x | 0.03 s | 2.7 s | 3.0 s | ~100x |
| `lsat_model` | 37.691 us | 88.382 us | 2.34x | 2.40 s | 3.6 s | 8.2 s | ~3.4x |
| `Mh_model` | 14.986 us | 34.912 us | 2.33x | 1.10 s | 3.1 s | 5.8 s | ~5.3x |
| `ldaK2` | 48.743 us | 112.432 us | 2.31x | 1.27 s | 4.5 s | 7.8 s | ~6.1x |
| `kidscore_momhs` | 1.515 us | 3.476 us | 2.29x | 0.06 s | 2.7 s | 3.1 s | ~51x |
| `radon_county` | 34.568 us | 79.100 us | 2.29x | 2.01 s | 3.0 s | 7.5 s | ~3.7x |
| `kidscore_momiq` | 1.520 us | 3.439 us | 2.26x | 0.13 s | 2.8 s | 3.3 s | ~25x |
| `hierarchical_gp` | 19.566 us | 41.136 us | 2.10x | 13.86 s | 8.7 s | 26.1 s | ~1.9x |
| `kilpisjarvi` | 317 ns | 663 ns | 2.09x | 0.71 s | 2.8 s | 4.5 s | ~6.3x |
| `seeds_centered_model` | 705 ns | 1.472 us | 2.09x | 0.07 s | 3.7 s | 4.0 s | ~58x |
| `hier_2pl` | 191.319 us | 398.265 us | 2.08x | 12.31 s | 6.5 s | 33.3 s | ~2.7x |
| `irt_2pl` | 16.303 us | 33.717 us | 2.07x | 1.27 s | 4.0 s | 6.3 s | ~5.0x |
| `normal_mixture` | 42.438 us | 87.584 us | 2.06x | 0.43 s | 2.5 s | 3.7 s | ~8.5x |
| `logmesquite_logvolume` | 244 ns | 499 ns | 2.05x | 0.02 s | 2.9 s | 3.2 s | ~160x |
| `2pl_latent_reg_irt` | 64.654 us | 132.198 us | 2.04x | 4.87 s | 5.7 s | 13.6 s | ~2.8x |
| `low_dim_gauss_mix_collapse` | 46.476 us | 94.929 us | 2.04x | 2.22 s | 3.0 s | 7.5 s | ~3.4x |
| `low_dim_gauss_mix` | 48.422 us | 97.930 us | 2.02x | 0.80 s | 3.1 s | 5.0 s | ~6.2x |
| `losscurve_sislob` | 1.055 us | 2.125 us | 2.01x | 0.16 s | 4.1 s | 4.5 s | ~28x |
| `prophet` | 34.943 us | 69.513 us | 1.99x | 59.63 s | 4.7 s | 122.7 s | ~2.1x |
| `accel_gp` | 4.446 us | 8.756 us | 1.97x | 8.96 s | 5.1 s | 22.0 s | ~2.4x |
| `accel_splines` | 4.681 us | 9.156 us | 1.96x | 10.76 s | 4.2 s | 23.6 s | ~2.2x |
| `iohmm_reg` | 166.418 us | 324.005 us | 1.95x | 85.73 s | 5.9 s | 186.5 s | ~2.2x |
| `soil_incubation` | 30.783 us | 58.876 us | 1.91x | 6.30 s | 3.3 s | 16.3 s | ~2.6x |
| `lotka_volterra` | 21.627 us | 40.781 us | 1.89x | 2.19 s | 4.1 s | 10.5 s | ~4.8x |
| `normal_mixture_k` | 191.468 us | 358.560 us | 1.87x | 60.76 s | 3.3 s | 105.9 s | ~1.7x |
| `wells_dist` | 21.099 us | 38.603 us | 1.83x | 0.65 s | 2.8 s | 4.3 s | ~6.6x |
| `seeds_model` | 687 ns | 1.229 us | 1.79x | 0.08 s | 3.5 s | 3.8 s | ~48x |
| `blr` | 505 ns | 898 ns | 1.78x | 0.05 s | 3.0 s | 3.3 s | ~66x |
| `seeds_stanified_model` | 679 ns | 1.199 us | 1.77x | 0.07 s | 3.4 s | 3.7 s | ~53x |
| `dugongs_model` | 525 ns | 927 ns | 1.77x | 0.05 s | 2.9 s | 3.2 s | ~64x |
| `hmm_example` | 16.074 us | 26.172 us | 1.63x | 0.48 s | 3.9 s | 4.9 s | ~10x |
| `eight_schools_noncentered` | 215 ns | 350 ns | 1.63x | 0.03 s | 3.1 s | 3.4 s | ~110x |
| `covid19imperial_v3` | 233.626 us | 378.892 us | 1.62x | 119.99 s | 8.2 s | 189.6 s | ~1.6x |
| `covid19imperial_v2` | 234.574 us | 370.830 us | 1.58x | 120.43 s | 7.2 s | 186.9 s | ~1.6x |
| `GLM_Binomial_model` | 815 ns | 1.268 us | 1.56x | 0.05 s | 3.1 s | 3.4 s | ~67x |
| `hmm_gaussian` | 168.724 us | 261.923 us | 1.55x | 282.47 s | 4.6 s | 23.4 s | ~0.08x |
| `pilots` | 579 ns | 881 ns | 1.52x | 0.31 s | 3.2 s | 4.5 s | ~15x |
| `eight_schools_centered` | 217 ns | 323 ns | 1.49x | 0.04 s | 2.9 s | 3.2 s | ~79x |
| `Mb_model` | 34.017 us | 48.364 us | 1.42x | 0.87 s | 3.1 s | 4.2 s | ~4.9x |
| `Rate_1_model` | 69 ns | 95 ns | 1.38x | 0.01 s | 2.3 s | 2.5 s | ~250x |
| `Rate_4_model` | 105 ns | 144 ns | 1.37x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `Rate_3_model` | 85 ns | 115 ns | 1.35x | 0.02 s | 2.3 s | 2.5 s | ~130x |
| `Rate_5_model` | 85 ns | 114 ns | 1.34x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `hmm_drive_1` | 111.170 us | 143.740 us | 1.29x | 4.35 s | 4.6 s | 11.6 s | ~2.7x |
| `hmm_drive_0` | 102.358 us | 132.266 us | 1.29x | 2.96 s | 4.6 s | 8.3 s | ~2.8x |
| `surgical_model` | 448 ns | 575 ns | 1.28x | 0.04 s | 3.3 s | 3.6 s | ~90x |
| `Rate_2_model` | 130 ns | 165 ns | 1.27x | 0.02 s | 2.4 s | 2.6 s | ~130x |
| `arma11` | 4.131 us | 5.232 us | 1.27x | 0.08 s | 2.8 s | 3.1 s | ~39x |
| `gp_regr` | 2.669 us | 3.256 us | 1.22x | 0.51 s | 5.2 s | 5.5 s | ~11x |
| `bones_model` | 41.373 us | 49.977 us | 1.21x | 0.70 s | 3.3 s | 4.7 s | ~6.7x |
| `gp_pois_regr` | 2.274 us | 2.667 us | 1.17x | 0.77 s | 5.5 s | 7.1 s | ~9.3x |
| `garch11` | 6.900 us | 7.938 us | 1.15x | 0.19 s | 2.8 s | 3.3 s | ~17x |
| `kronecker_gp` | 191.553 us | 217.868 us | 1.14x | 388.59 s | 8.0 s | 454.8 s | ~1.2x |
| `Survey_model` | 53.720 us | 60.638 us | 1.13x | 1.12 s | 2.8 s | 3.9 s | ~3.5x |
| `nn_rbm1bJ10` | 168.674 us | 184.515 us | 1.09x | 426.60 s | 5.2 s | 458.2 s | ~1.1x |
| `wells_interaction_c_model` | 18.831 us | 19.711 us | 1.05x | 0.26 s | 3.2 s | 3.7 s | ~14x |
| `wells_dae_c_model` | 17.520 us | 18.332 us | 1.05x | 0.35 s | 3.2 s | 3.8 s | ~11x |
| `wells_daae_c_model` | 19.174 us | 20.019 us | 1.04x | 0.42 s | 3.2 s | 3.8 s | ~9.1x |
| `wells_dist100_model` | 15.947 us | 16.643 us | 1.04x | 0.25 s | 3.0 s | 3.5 s | ~14x |
| `nes_logit_model` | 6.126 us | 6.381 us | 1.04x | 0.15 s | 3.0 s | 3.4 s | ~23x |
| `wells_interaction_model` | 18.787 us | 19.505 us | 1.04x | 0.69 s | 3.1 s | 4.0 s | ~5.9x |
| `wells_dist100ars_model` | 17.280 us | 17.914 us | 1.04x | 0.39 s | 3.0 s | 3.6 s | ~9.4x |
| `wells_dae_model` | 18.734 us | 19.365 us | 1.03x | 0.55 s | 3.1 s | 3.9 s | ~7.1x |
| `wells_dae_inter_model` | 19.546 us | 20.202 us | 1.03x | 0.32 s | 3.2 s | 3.8 s | ~12x |
| `diamonds` | 31.069 us | 31.897 us | 1.03x | 49.65 s | 3.5 s | 52.4 s | ~1.1x |
| `one_comp_mm_elim_abs` | 459.111 us | 462.810 us | 1.01x | 9.27 s | 3.3 s | 14.5 s | ~1.6x |

120 models; 119 with both gradients; median per-gradient speedup 2.10x; 119/119
at or above CmdStan. 117 completed first runs; median source-to-CSV speedup
about 8.6x; 116/117 at or above CmdStan including its model build.

The extreme `hmm_gaussian` first-run result is not a useful speed comparison:
every post-warmup draw in CmdStan's retained seed-1 run was divergent, so the
two engines did radically different sampling work. Its controlled gradient
row, 1.60x in stanli's favor, is the meaningful result.

### Runs that did not complete

A missing run time is not a missing gradient. These rows sort below the
complete table, and their measured gradient ratios still stand.

| model | stanli gradient | CmdStan gradient | gradient speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 2.287 ms | 5.506 ms | 2.41x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `nn_rbm1bJ100` | 413.915 ms | 435.557 ms | 1.05x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `sir` | - | - | - | stanli's gradient probe threw at the benchmark point; the CmdStan gradient driver would not run; no stanli gradient |

`sir` has no gradient number because the fixed probe point makes its ODE
solution dip to -4.4e-10 and `poisson_lpmf` rejects the negative rate. The
model itself samples successfully. `ldaK5` and `nn_rbm1bJ100` completed both
gradient probes before both sampling commands reached the 900 s cap.

## Against CmdStan with stanc3's optimizer and loop vectorizer

The tables above build CmdStan the way most users build it, with `stanc` and
no extra flags. This section instead builds CmdStan with `stanc3` at the
pinned commit `8e154ac34`, patched so `--O1` also runs `vectorize_loops`, the
same pass stanli's own pipeline runs by default (see
[`compiler/ocaml/stanli_pipeline.ml`](../../compiler/ocaml/stanli_pipeline.ml)).
It uses the same host and driver as the main table above. Only gradient and
compile time are measured; there is no sampling column.

The gradient speedup is CmdStan's time per gradient evaluation divided by
stanli's, at the same unconstrained point. The compile+sample speedup
estimates a short run: each engine's compile time plus two thousand gradient
evaluations, CmdStan's total divided by stanli's. Two thousand gradients is
roughly what a few hundred NUTS iterations cost, so for most models CmdStan's
per-model build dominates and the column tracks close to the compile-time
ratio; it approximates the wait for a first short run, not sampler
throughput.

CmdStan's build time here uses the same warm precompiled-header path as the
main table.

<!-- corpus_table.py docs/corpus-bench-o1vec.tsv --o1vec -->
| model | gradient speedup | stanli compile | CmdStan compile | compile+sample speedup |
| --- | ---: | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 11.26x | 0.090 s | 5.3 s | ~24x |
| `grsm_latent_reg_irt` | 10.53x | 0.030 s | 5.1 s | ~39x |
| `radon_partially_pooled_centered` | 7.81x | 0.030 s | 2.9 s | ~35x |
| `radon_partially_pooled_noncentered` | 7.76x | 0.030 s | 3.1 s | ~36x |
| `radon_hierarchical_intercept_centered` | 6.89x | 0.059 s | 3.1 s | ~21x |
| `radon_county_intercept` | 6.85x | 0.037 s | 2.8 s | ~26x |
| `radon_variable_intercept_noncentered` | 6.81x | 0.038 s | 3.1 s | ~27x |
| `radon_hierarchical_intercept_noncentered` | 6.80x | 0.060 s | 3.2 s | ~21x |
| `radon_variable_intercept_centered` | 6.76x | 0.038 s | 2.9 s | ~26x |
| `radon_variable_slope_noncentered` | 6.63x | 0.038 s | 3.1 s | ~27x |
| `radon_variable_slope_centered` | 6.63x | 0.039 s | 2.9 s | ~25x |
| `radon_variable_intercept_slope_centered` | 6.12x | 0.041 s | 3.0 s | ~24x |
| `radon_variable_intercept_slope_noncentered` | 6.11x | 0.042 s | 3.3 s | ~25x |
| `arK` | 4.83x | 0.002 s | 2.7 s | ~490x |
| `dogs` | 4.49x | 0.014 s | 4.0 s | ~150x |
| `dogs_log` | 4.41x | 0.014 s | 3.6 s | ~140x |
| `Mth_model` | 4.31x | 0.018 s | 4.1 s | ~66x |
| `rats_model` | 4.18x | 0.001 s | 2.9 s | ~950x |
| `Mt_model` | 3.88x | 0.002 s | 3.3 s | ~310x |
| `M0_model` | 3.35x | 0.002 s | 2.6 s | ~250x |
| `dogs_hierarchical` | 3.20x | 0.013 s | 2.6 s | ~80x |
| `GLMM1_model` | 3.11x | 0.004 s | 3.1 s | ~130x |
| `Mtbh_model` | 3.08x | 0.017 s | 4.7 s | ~110x |
| `state_space_stochastic_level_stochastic_seasonal` | 2.94x | 0.002 s | 4.6 s | ~320x |
| `logmesquite` | 2.74x | 0.000 s | 3.2 s | ~2370x |
| `logmesquite_logvas` | 2.73x | 0.000 s | 3.5 s | ~2580x |
| `mesquite` | 2.69x | 0.000 s | 3.2 s | ~2410x |
| `nes` | 2.66x | 0.003 s | 3.4 s | ~99x |
| `dogs_nonhierarchical` | 2.59x | 0.014 s | 6.5 s | ~150x |
| `logmesquite_logvash` | 2.59x | 0.000 s | 3.4 s | ~2700x |
| `ldaK5` | 2.43x | 0.441 s | 3.5 s | ~2.9x |
| `kidscore_mom_work` | 2.35x | 0.001 s | 3.0 s | ~490x |
| `kidscore_interaction_c2` | 2.34x | 0.000 s | 3.0 s | ~560x |
| `multi_occupancy` | 2.34x | 0.009 s | 6.1 s | ~120x |
| `kidscore_interaction` | 2.33x | 0.000 s | 3.0 s | ~560x |
| `kidscore_interaction_c` | 2.32x | 0.000 s | 3.0 s | ~560x |
| `logmesquite_logva` | 2.31x | 0.000 s | 3.2 s | ~3040x |
| `kidscore_interaction_z` | 2.30x | 0.001 s | 3.1 s | ~580x |
| `logearn_interaction_z` | 2.23x | 0.001 s | 3.1 s | ~210x |
| `logearn_interaction` | 2.21x | 0.001 s | 3.0 s | ~200x |
| `Mh_model` | 2.20x | 0.006 s | 3.1 s | ~87x |
| `logearn_logheight_male` | 2.16x | 0.001 s | 2.9 s | ~260x |
| `logearn_height_male` | 2.16x | 0.001 s | 2.9 s | ~260x |
| `radon_county` | 2.12x | 0.007 s | 3.0 s | ~41x |
| `hierarchical_gp` | 2.11x | 0.014 s | 8.6 s | ~160x |
| `low_dim_gauss_mix` | 2.10x | 0.004 s | 3.1 s | ~33x |
| `normal_mixture` | 2.08x | 0.003 s | 2.7 s | ~33x |
| `low_dim_gauss_mix_collapse` | 2.05x | 0.004 s | 3.0 s | ~33x |
| `GLM_Poisson_model` | 2.05x | 0.000 s | 3.1 s | ~2870x |
| `election88_full` | 2.03x | 0.008 s | 4.8 s | ~13x |
| `kidscore_momhsiq` | 2.01x | 0.000 s | 2.9 s | ~640x |
| `ldaK2` | 1.99x | 0.009 s | 3.5 s | ~35x |
| `soil_incubation` | 1.97x | 0.001 s | 3.7 s | ~67x |
| `GLMM_Poisson_model` | 1.90x | 0.000 s | 3.6 s | ~2080x |
| `normal_mixture_k` | 1.86x | 0.035 s | 3.3 s | ~9.6x |
| `seeds_centered_model` | 1.86x | 0.000 s | 3.7 s | ~2050x |
| `dugongs_model` | 1.84x | 0.000 s | 2.9 s | ~2020x |
| `lotka_volterra` | 1.84x | 0.001 s | 4.3 s | ~99x |
| `iohmm_reg` | 1.84x | 0.469 s | 5.9 s | ~8.1x |
| `radon_pooled` | 1.81x | 0.006 s | 2.7 s | ~31x |
| `losscurve_sislob` | 1.79x | 0.002 s | 4.4 s | ~1010x |
| `log10earn_height` | 1.78x | 0.001 s | 2.8 s | ~330x |
| `kidscore_momhs` | 1.77x | 0.000 s | 2.8 s | ~820x |
| `sesame_one_pred_a` | 1.77x | 0.000 s | 2.7 s | ~1370x |
| `logearn_height` | 1.76x | 0.001 s | 2.8 s | ~330x |
| `earn_height` | 1.74x | 0.001 s | 2.7 s | ~320x |
| `kidscore_momiq` | 1.72x | 0.000 s | 2.8 s | ~820x |
| `bym2_offset_only` | 1.72x | 0.002 s | 4.2 s | ~59x |
| `accel_gp` | 1.71x | 0.004 s | 6.3 s | ~490x |
| `logmesquite_logvolume` | 1.67x | 0.000 s | 2.9 s | ~3520x |
| `kilpisjarvi` | 1.64x | 0.000 s | 2.7 s | ~2960x |
| `hmm_example` | 1.62x | 0.020 s | 3.9 s | ~76x |
| `lsat_model` | 1.59x | 0.002 s | 3.6 s | ~49x |
| `hmm_gaussian` | 1.55x | 0.301 s | 4.6 s | ~8.0x |
| `accel_splines` | 1.54x | 0.003 s | 4.3 s | ~330x |
| `hier_2pl` | 1.53x | 0.010 s | 6.4 s | ~18x |
| `2pl_latent_reg_irt` | 1.52x | 0.004 s | 5.5 s | ~42x |
| `Mb_model` | 1.50x | 0.021 s | 3.1 s | ~36x |
| `covid19imperial_v3` | 1.47x | 0.657 s | 6.5 s | ~6.4x |
| `wells_dist` | 1.46x | 0.001 s | 2.9 s | ~68x |
| `seeds_model` | 1.46x | 0.000 s | 3.5 s | ~1950x |
| `seeds_stanified_model` | 1.46x | 0.000 s | 3.5 s | ~2010x |
| `covid19imperial_v2` | 1.45x | 0.657 s | 6.5 s | ~6.4x |
| `prophet` | 1.45x | 0.008 s | 5.3 s | ~69x |
| `Rate_4_model` | 1.41x | 0.000 s | 2.4 s | ~5560x |
| `Rate_1_model` | 1.37x | 0.000 s | 2.3 s | ~6890x |
| `eight_schools_noncentered` | 1.36x | 0.000 s | 3.1 s | ~4620x |
| `GLM_Binomial_model` | 1.34x | 0.000 s | 3.1 s | ~1520x |
| `pilots` | 1.33x | 0.000 s | 3.3 s | ~2060x |
| `irt_2pl` | 1.31x | 0.001 s | 4.0 s | ~120x |
| `hmm_drive_1` | 1.30x | 0.091 s | 4.5 s | ~15x |
| `Rate_3_model` | 1.30x | 0.000 s | 2.3 s | ~5910x |
| `surgical_model` | 1.28x | 0.000 s | 3.2 s | ~2690x |
| `Rate_2_model` | 1.28x | 0.000 s | 2.4 s | ~4560x |
| `hmm_drive_0` | 1.28x | 0.090 s | 4.6 s | ~16x |
| `Rate_5_model` | 1.27x | 0.000 s | 2.4 s | ~5960x |
| `eight_schools_centered` | 1.25x | 0.000 s | 2.8 s | ~3970x |
| `bones_model` | 1.22x | 0.011 s | 3.4 s | ~37x |
| `logistic_regression_rhs` | 1.19x | 0.012 s | 4.8 s | ~53x |
| `gp_regr` | 1.19x | 0.000 s | 5.1 s | ~890x |
| `Survey_model` | 1.13x | 0.006 s | 2.8 s | ~26x |
| `kronecker_gp` | 1.12x | 0.007 s | 9.0 s | ~24x |
| `gp_pois_regr` | 1.11x | 0.000 s | 5.4 s | ~1090x |
| `nn_rbm1bJ10` | 1.11x | 0.009 s | 5.2 s | ~16x |
| `wells_dist100_model` | 1.06x | 0.001 s | 3.0 s | ~93x |
| `nn_rbm1bJ100` | 1.05x | 3.018 s | 5.1 s | ~1.1x |
| `nes_logit_model` | 1.05x | 0.000 s | 3.0 s | ~240x |
| `wells_dist100ars_model` | 1.05x | 0.001 s | 3.0 s | ~85x |
| `wells_dae_c_model` | 1.04x | 0.002 s | 3.2 s | ~88x |
| `wells_interaction_model` | 1.04x | 0.002 s | 3.1 s | ~81x |
| `wells_dae_model` | 1.04x | 0.002 s | 3.1 s | ~81x |
| `wells_interaction_c_model` | 1.03x | 0.002 s | 3.1 s | ~81x |
| `wells_dae_inter_model` | 1.03x | 0.002 s | 3.2 s | ~79x |
| `wells_daae_c_model` | 1.03x | 0.002 s | 3.2 s | ~80x |
| `one_comp_mm_elim_abs` | 1.01x | 0.001 s | 3.6 s | ~4.9x |
| `diamonds` | 1.00x | 0.022 s | 3.2 s | ~39x |
| `arma11` | 0.94x | 0.003 s | 2.8 s | ~260x |
| `garch11` | 0.94x | 0.003 s | 2.8 s | ~170x |
| `blr` | 0.82x | 0.000 s | 3.0 s | ~2080x |

| model | stanli gradient | CmdStan gradient | gradient speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `sir` | - | - | - | stanli's gradient probe threw at the benchmark point; the CmdStan gradient driver would not run; no stanli compile time; no stanli gradient |

## Benchmark method

This section describes the September 11 posteriordb tables; the stanc3
optimizer and loop vectorizer comparison uses the same host and driver, with
its own build and stanc flags described in the previous section.

Measured 2026-09-11 on an Apple M3 Ultra (macOS arm64) with Apple clang 21,
single-threaded, with both engines built at `-O3` and
`-ffp-contract=off`. The original columns come from one 120-model run per engine. The stanli
rows for `gp_regr`, `gp_pois_regr`, and `hierarchical_gp` were refreshed on
2026-09-11 at `de0bd757` using the Release candidate and the corpus harness's
`--stanli-only` workflow, including fresh 1,000-warmup/1,000-draw runs. The
same measured stanli preparation and gradient cells are used in the optimizer
comparison, which uses the same stanli pipeline. CmdStan and unrelated model
cells retain their prior measurements. `accel_gp` uses a spectral approximation
and does not execute the affected pullbacks. The CmdStan
columns were re-measured because the gradient driver's warmup changed on
2026-08-29 from a fixed count to a fixed duration, and the earlier column
overstated CmdStan's time on the smallest models. CmdStan's one-time `make
build` setup was already complete, so its per-model build column uses the
warm precompiled header path and does not include that earlier setup cost.

For gradients, both engines evaluate the sampling log density (proportional
terms plus Jacobian) at the same deterministic unconstrained point. stanli
runs `tools/bench_grad.cpp`; CmdStan runs `tools/bench_cmdstan_grad.cpp` over
the stanc-generated model. CmdStan's driver performs the fresh-vars, gradient,
and memory recovery cycle a leapfrog step performs against that model.
stanli's driver performs the `stan::model::gradient` call the sampler makes
against the executor. The reported cells are warmed arithmetic means from one
timed loop per model.

For complete runs, `stanli_sample_s` in
[`corpus-bench.tsv`](../../docs/corpus-bench.tsv) measures the entire `stanli_run`
process from Stan source to CSV. CmdStan's build and execution are timed
separately, so the displayed total adds `cmdstan_build_s` and
`cmdstan_sample_s`. The sampler rows are real wall-clock observations, not a
fixed-work microbenchmark; use the gradient rows when comparing engine
throughput independent of a particular NUTS trajectory.

## For developers

The implementation story is intentionally elsewhere. For a conceptual
overview, read [How stanli works](../../docs/how-it-works.md). For graph re-rolling, lane
partitioning, generated adjoints, tape islands, the compiled ODE right-hand
side, and their targeted A/B measurements, read
[Graph optimizations and performance work](../../runtime/src/OPTIMIZATIONS.md).

## New measurements

Use a fresh output for paired measurements. The historical tables and archived experiments remain
unchanged; the current runner cannot append to them or refresh only one engine.


```sh
./tools/dev_setup.sh --corpus          # deps, build, posteriordb, CmdStan
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/corpus-v3.tsv --sampling --sample-timeout 900
python3 tools/corpus_table.py /tmp/corpus-v3.tsv
python3 harnesses/ab_corpus.py deps/posteriordb
```

The stanc3 optimizer and loop vectorizer table comes from a separate run
against a patched stanc. The patch is a stanc3 checkout at the pinned commit
with `vectorize_loops` enabled at `--O1` in its optimization level table; see
[`compiler/ocaml/stanli_pipeline.ml`](../../compiler/ocaml/stanli_pipeline.ml)
for where stanli's own pipeline does the same.

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/corpus-v3-o1vec.tsv --cmdstan-stanc PATH_TO_PATCHED_STANC \
  --stancflags=--O1 --gradient-timeout 900
python3 tools/corpus_table.py /tmp/corpus-v3-o1vec.tsv
```
