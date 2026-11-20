# Corpus benchmark evidence

Run `881dbca09738e257`, started 2026-11-20T00:16:22Z, records 61 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/Users/xitrium/claud/stanrt/.worktrees/bench-0290/build/corpus-current/rerun.tsv.run`; these larger artifacts are not hosted with this report.

Run `881dbca09738e257` (2026-11-20): 61 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 61 of 61 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `s2_multinomial` | 5.53x | 12.9k | 0.0358 | 3.04 | 85x |
| `sw_nonlinear` | 5.43x | 10.9k | 0.0135 | 2.81 | 208x |
| `s2_nl_noloop` | 4.17x | 52.6k | 0.0339 | 2.94 | 86.8x |
| `sw_me` | 3.88x | 57.9k | 0.0411 | 3.16 | 76.9x |
| `sw_dist_sigma` | 3.84x | 10.7k | 0.0157 | 2.75 | 176x |
| `s2_me2_nomecor` | 3.79x | 50.4k | 0.0567 | 3.49 | 61.6x |
| `s2_s_cc` | 3.75x | 30.5k | 0.0213 | 3.25 | 153x |
| `s2_nlf` | 3.61x | 9.42k | 0.0167 | 2.9 | 173x |
| `s2_mm` | 3.42x | 26.6k | 0.0271 | 3.66 | 135x |
| <code>state_space_stochastic_</code><br><code>level_stochastic_</code><br><code>seasonal</code> | 3.4x | 1.03M | 6.62 | 26.2 | 3.96x |
| `s2_mm_weights` | 3.39x | 27.8k | 0.0277 | 3.66 | 132x |
| `s2_s_by` | 3.37x | 70.7k | 0.115 | 5.22 | 45.3x |
| `sw_lognormal` | 3.35x | 8.28k | 0.0119 | 2.61 | 220x |
| `sw_re_gauss` | 3.01x | 29.1k | 0.0247 | 3.53 | 143x |
| `s2_mi_lognormal` | 2.91x | 14.8k | 0.0205 | 3.56 | 174x |
| `sw_hurdle_lognormal` | 2.83x | 12.5k | 0.0243 | 2.5 | 103x |
| `sw_acat` | 2.79x | 12.2k | 0.679 | 4.62 | 6.81x |
| `sesame_one_pred_a` | 2.78x | 15.5k | 0.0194 | 1.64 | 84.8x |
| `sw_acat_cs` | 2.76x | 18.3k | 1.21 | 7.21 | 5.96x |
| `s2_mmc` | 2.74x | 35.4k | 0.0616 | 6.16 | 100x |
| `sw_gamma` | 2.68x | 9.45k | 0.0144 | 2.65 | 184x |
| `sw_hurdle_pois` | 2.57x | 10.4k | 0.0275 | 2.29 | 83.3x |
| `s2_me2` | 2.55x | 71.6k | 0.172 | 6.08 | 35.3x |
| `s2_sar` | 2.52x | 10.9k | 0.0499 | 3.09 | 61.9x |
| `s2_rate` | 2.39x | 7.8k | 0.0133 | 2.52 | 189x |
| `sw_re_bern` | 2.35x | 27.4k | 0.0279 | 3.64 | 131x |
| `s2_mv_shared_re` | 2.28x | 36.8k | 0.0659 | 6.3 | 95.7x |
| `sw_re_pois` | 2.07x | 30k | 0.0343 | 3.47 | 101x |
| `sw_cens` | 2.01x | 10.8k | 0.0235 | 3.25 | 138x |
| `seeds_stanified_model` | 2x | 42.2k | 0.0364 | 2.49 | 68.2x |
| `sw_hurdle_gamma` | 1.98x | 13.3k | 0.0356 | 2.5 | 70x |
| `sw_exgaussian` | 1.98x | 25.1k | 0.0322 | 2.85 | 88.4x |
| `soil_incubation` | 1.94x | 203k | 5.02 | 12 | 2.39x |
| `sw_mi` | 1.85x | 15.1k | 0.0193 | 2.78 | 144x |
| `sw_arma` | 1.84x | 42.2k | 0.107 | 3.46 | 32.4x |
| `sw_ar` | 1.8x | 10.4k | 0.0272 | 3.2 | 118x |
| `sw_ma` | 1.76x | 11.1k | 0.0297 | 3.21 | 108x |
| `sw_beta` | 1.73x | 8.99k | 0.0256 | 2.82 | 110x |
| `s2_mv_subset` | 1.72x | 12.1k | 0.0155 | 2.36 | 153x |
| `sw_asymlaplace` | 1.72x | 23.4k | 0.0573 | 2.51 | 43.8x |
| `sw_mv_norescor` | 1.63x | 11.6k | 0.0163 | 2.36 | 145x |
| `sw_gaussian` | 1.62x | 10.9k | 0.0116 | 2.17 | 188x |
| `s2_mixture_theta` | 1.61x | 80.4k | 0.46 | 4.17 | 9.06x |
| `surgical_model` | 1.6x | 22.2k | 0.0169 | 2.29 | 135x |
| `sir` | 1.58x | 127k | 21.1 | 36.4 | 1.72x |
| `sw_re_negbin` | 1.48x | 30.3k | 0.0684 | 3.77 | 55.2x |
| `sw_mono` | 1.35x | 20.4k | 0.0419 | 2.94 | 70.1x |
| `sw_binomial` | 1.34x | 7.52k | 0.0227 | 2.46 | 108x |
| `sw_mv_rescor` | 1.33x | 13.3k | 0.084 | 5.34 | 63.7x |
| `sw_cratio_cs` | 1.32x | 13.7k | 1.12 | 5.11 | 4.56x |
| `sw_mixture` | 1.32x | 303k | 1.62 | 5.83 | 3.6x |
| `sw_gp` | 1.29x | 47.3k | 0.948 | 7.33 | 7.73x |
| `s2_mo_simo_prior` | 1.27x | 21.3k | 0.0461 | 2.93 | 63.5x |
| `sw_cratio` | 1.22x | 10.4k | 0.646 | 3.26 | 5.05x |
| `sw_bernoulli` | 1.2x | 8.63k | 0.012 | 2.13 | 177x |
| `sw_cumulative_cs` | 1.19x | 22.3k | 1.22 | 5.14 | 4.22x |
| `sw_poisson` | 1.16x | 9.85k | 0.0142 | 1.97 | 139x |
| `sw_categorical` | 1.11x | 11k | 0.0229 | 3.26 | 142x |
| `sw_negbinomial` | 1.07x | 1.49M | 2.15 | 4.55 | 2.12x |
| `s2_mi_trunc_lb` | 1.01x | 37.9k | 0.217 | 3.61 | 16.6x |
| `sw_cumulative` | 0.982x | 9.89k | 0.236 | 2.89 | 12.2x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
