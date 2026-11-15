# Corpus benchmark evidence

Run `927990d5d0dcee9d`, started 2026-11-14T23:30:56Z, records 51 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/464f5da1-aa71-4efb-be11-cd1664cf85f7/scratchpad/bench-publish/vecf-sw.tsv.run`; these larger artifacts are not hosted with this report.

Run `927990d5d0dcee9d` (2026-11-14): 51 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 51 of 51 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `sw_weights` | 3.55x | 9.98k | 0.0138 | 2.26 | 164x |
| `sw_me` | 3.35x | 57.9k | 0.0408 | 3.15 | 77.3x |
| `sw_re_gauss` | 3.11x | 29.1k | 0.0235 | 3.42 | 146x |
| `sw_acat_cs` | 2.78x | 18.3k | 1.25 | 7.32 | 5.85x |
| `sw_acat` | 2.74x | 12.2k | 0.634 | 4.33 | 6.83x |
| `sw_hurdle_lognormal` | 2.68x | 12.5k | 0.0231 | 2.39 | 104x |
| `sw_hurdle_pois` | 2.51x | 10.4k | 0.0265 | 2.25 | 85.1x |
| `sw_spline_s` | 2.39x | 37.6k | 0.0266 | 3.21 | 121x |
| `sw_re_bern` | 2.35x | 27.4k | 0.0284 | 3.5 | 123x |
| `sw_re_slope` | 2.33x | 53.4k | 0.0672 | 5.81 | 86.5x |
| `sw_nonlinear` | 2.23x | 10.9k | 0.0133 | 2.7 | 203x |
| `sw_vonmises` | 2.23x | 730k | 0.514 | 3.56 | 6.93x |
| `sw_skewnormal` | 2.22x | 15.5k | 0.0257 | 2.74 | 107x |
| `sw_lognormal` | 2.17x | 8.28k | 0.011 | 2.43 | 220x |
| `sw_re_pois` | 2.13x | 30k | 0.0326 | 3.36 | 103x |
| `sw_student` | 2.04x | 12.7k | 0.0153 | 2.6 | 169x |
| `sw_se` | 1.98x | 7.2k | 0.00995 | 2.23 | 224x |
| `sw_dist_sigma` | 1.95x | 10.7k | 0.0161 | 2.55 | 158x |
| `sw_spline_t2` | 1.95x | 38.1k | 0.0456 | 3.68 | 80.7x |
| `sw_zi_poisson` | 1.93x | 11.5k | 0.0232 | 2.23 | 96x |
| `sw_mi` | 1.8x | 15.1k | 0.0197 | 2.66 | 135x |
| `sw_hurdle_gamma` | 1.79x | 13.3k | 0.0359 | 2.36 | 65.7x |
| `sw_asymlaplace` | 1.77x | 23.4k | 0.0578 | 2.48 | 42.8x |
| `sw_zi_binomial` | 1.69x | 10.2k | 0.0311 | 2.23 | 71.7x |
| `sw_ar` | 1.63x | 10.4k | 0.0262 | 3.04 | 116x |
| `sw_cratio_cs` | 1.63x | 13.7k | 1.14 | 5.66 | 4.97x |
| `sw_arma` | 1.62x | 42.2k | 0.103 | 3.29 | 31.9x |
| `sw_ma` | 1.61x | 11.1k | 0.0289 | 3.06 | 106x |
| `sw_gaussian` | 1.58x | 10.9k | 0.0109 | 2.09 | 191x |
| `sw_cens` | 1.56x | 10.8k | 0.0241 | 3.06 | 127x |
| `sw_mv_norescor` | 1.53x | 11.6k | 0.0167 | 2.3 | 138x |
| `sw_re_negbin` | 1.52x | 30.3k | 0.0655 | 3.66 | 55.8x |
| `sw_gamma` | 1.52x | 9.45k | 0.0147 | 2.57 | 174x |
| `sw_zi_negbin` | 1.44x | 20.5k | 0.0707 | 2.36 | 33.3x |
| `sw_exgaussian` | 1.44x | 25.1k | 0.0314 | 2.7 | 86.2x |
| `sw_cumulative_cs` | 1.4x | 22.3k | 1.39 | 5.77 | 4.15x |
| `sw_mixture` | 1.3x | 303k | 1.59 | 5.53 | 3.48x |
| `sw_beta` | 1.29x | 8.99k | 0.0242 | 2.71 | 112x |
| `sw_gp` | 1.28x | 47.3k | 0.906 | 7.27 | 8.03x |
| `sw_mv_rescor` | 1.21x | 13.3k | 0.0892 | 5.04 | 56.5x |
| `sw_categorical` | 1.2x | 11k | 0.0216 | 3.15 | 146x |
| `sw_weibull` | 1.16x | 10.3k | 0.0281 | 2.71 | 96.6x |
| `sw_bernoulli` | 1.16x | 8.63k | 0.0109 | 2.07 | 190x |
| `sw_binomial` | 1.16x | 7.52k | 0.0211 | 2.29 | 109x |
| `sw_poisson` | 1.14x | 9.85k | 0.0135 | 1.9 | 141x |
| `sw_cratio` | 1.11x | 10.4k | 0.623 | 3.34 | 5.36x |
| `sw_sratio` | 1.1x | 10.4k | 0.629 | 3.37 | 5.36x |
| `sw_mono` | 1.09x | 20.4k | 0.0401 | 2.92 | 72.8x |
| `sw_trunc` | 1.09x | 27.2k | 0.175 | 2.52 | 14.4x |
| `sw_negbinomial` | 1.06x | 1.49M | 2.05 | 4.39 | 2.14x |
| `sw_cumulative` | 0.992x | 9.89k | 0.226 | 2.75 | 12.2x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
