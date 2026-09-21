# Teaching model performance

Recorded platform: macOS-26.6.2-arm64-arm-64bit; 32 logical CPUs. Source checkout: `6e462c2e2bb4bc297165b5640dfe30318345960d`. The manifest records tracked differences, configuration, dependencies, and executable hashes.

Run `fd5e0ecacddc7047` includes every one of its 199 fixtures. Failures and timeouts remain in the tables.

## Collection summary

| Collection | Fixtures | Completed in both | Lower Stanli CLI time | Median CLI sampling time ratio (CmdStan/Stanli) | Screen clear in both |
| --- | ---: | ---: | ---: | ---: | ---: |
| educational | 13 | 13 | 12/13 | 1.32 | 10/13 |
| rethinking | 62 | 62 | 62/62 | 1.54 | 50/62 |
| brms | 124 | 118 | 113/118 | 1.45 | 76/118 |

The summary compares end-to-end CLI sampling time, including Stanli preparation. A ratio above one means less elapsed time for Stanli. Each model has equal weight in the median; these ratios describe the completed fixtures only. Diagnostic flags are not removed from the timing summary.

## Practical timing target

The target is CmdStan/Stanli ≥ 0.8: Stanli takes at most 1.25× CmdStan’s complete CLI time. This is a timing target, not a claim of numerical correctness or reliable inference. Capped and failed runs remain unmeasured; the timeout is not the success threshold.

| Collection | Target met | Below target | Unmeasured |
| --- | ---: | ---: | ---: |
| educational | 13/13 | 0 | 0 |
| rethinking | 62/62 | 0 | 0 |
| brms | 114/124 | 4 | 6 |

## What was measured

Each engine ran 4 independent single-chain seeds, with 1000 warmup iterations and 1000 retained draws per seed, target acceptance 0.8, tree depth 10, and random initialization. The table reports median [minimum–maximum] CLI seconds. Stanli includes model preparation; CmdStan starts from a compiled model. Both include generated quantities and CSV output. Toolchain installation is excluded.

CSV precision follows the recorded CLI defaults. The numerical oracle uses separate high-precision values; sampling diagnostics use these retained CSVs.

CmdStan compilation is measured once, including Stan translation and the C++ model build. Adding it to the median CmdStan CLI time gives an estimated first fit; this is a sum of measured stages. It is not a directly timed four-chain R fit or a cold-cache measurement.

CmdStan ran first for each seed. Stanli's cap was min(3 × that CmdStan CLI time, 900.0 seconds). A failed, invalid, or capped seed prevents an aggregate for that engine. A preceding preparation or gradient-gate failure leaves sampling unmeasured. No partial-seed averages are substituted.

Screen clear: no retained-draw divergences or depth hits, finite R-hat ≤ 1.01 and bulk ESS ≥ 400 for every nonconstant variable in the parameters block. Structural fixed matrix entries are checked and omitted. The CSV also records tail ESS, minimum E-BFMI, and minimum bulk ESS divided by the sum of the four serial CLI durations. These are descriptive diagnostics; fixed-budget runtime is not time to equal inferential accuracy.

Gradient results in the CSV use six alternating pairs at the same parameter point. `paired_speedup` is the CmdStan/Stanli gradient time ratio; values above one favor Stanli. Each pair must satisfy the full density/gradient scaled-error gate of 1e-9. The separate three-point numerical replay and its known exceptions are described in the [support guide](../../docs/teaching-support.md).

## Full appendix

| Collection / fixture | Stanli CLI | CmdStan CLI | CLI ratio C/S | Timing target | CmdStan compile | Screen S/C |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| educational / aalto_bern | 0.012 [0.012–0.014] | 0.014 [0.014–0.213] | 1.17 | met | 2.59 | clear/clear |
| educational / aalto_binom | 0.011 [0.011–0.015] | 0.014 [0.012–0.219] | 1.21 | met | 2.38 | clear/clear |
| educational / aalto_binom2 | 0.013 [0.012–0.018] | 0.016 [0.013–0.202] | 1.28 | met | 2.51 | clear/clear |
| educational / aalto_binomb | 0.011 [0.011–0.011] | 0.012 [0.012–0.198] | 1.11 | met | 2.41 | clear/clear |
| educational / aalto_gpareto | 0.027 [0.025–0.029] | 0.025 [0.022–0.228] | 0.939 | met | 2.88 | clear/clear |
| educational / aalto_grp_aov | 0.018 [0.017–0.019] | 0.024 [0.024–0.222] | 1.33 | met | 3.10 | clear/clear |
| educational / aalto_grp_prior_mean | 0.028 [0.027–0.033] | 0.036 [0.033–0.231] | 1.30 | met | 3.17 | review/review |
| educational / aalto_grp_prior_mean_var | 0.061 [0.051–0.068] | 0.103 [0.089–0.292] | 1.70 | met | 3.92 | review/review |
| educational / aalto_lin | 0.022 [0.021–0.023] | 0.030 [0.028–0.215] | 1.35 | met | 3.09 | clear/review |
| educational / aalto_lin_std | 0.020 [0.018–0.021] | 0.026 [0.023–0.221] | 1.32 | met | 3.32 | clear/clear |
| educational / aalto_lin_std_t | 0.023 [0.021–0.023] | 0.030 [0.030–0.227] | 1.32 | met | 3.50 | clear/clear |
| educational / aalto_poisson_hurdle | 0.600 [0.589–0.602] | 0.827 [0.783–1.05] | 1.38 | met | 2.93 | clear/clear |
| educational / aalto_poisson_simple | 0.087 [0.087–0.089] | 0.170 [0.164–0.379] | 1.94 | met | 2.66 | clear/clear |
| rethinking / ch09_m5_8s | 1.22 [1.20–1.24] | 3.53 [3.44–3.66] | 2.89 | met | 2.85 | review/review |
| rethinking / ch09_m5_8s2 | 0.861 [0.308–1.18] | 2.59 [1.24–3.32] | 3.01 | met | 2.85 | review/review |
| rethinking / ch09_m9_1 | 0.029 [0.026–0.032] | 0.053 [0.046–0.252] | 1.82 | met | 3.08 | clear/clear |
| rethinking / ch09_m9_1_chains4 | 0.029 [0.028–0.031] | 0.054 [0.051–0.258] | 1.84 | met | 3.06 | clear/clear |
| rethinking / ch09_m9_2 | 0.017 [0.016–0.018] | 0.024 [0.022–0.222] | 1.41 | met | 2.51 | review/review |
| rethinking / ch09_m9_3 | 0.013 [0.012–0.017] | 0.018 [0.015–0.200] | 1.35 | met | 2.51 | clear/clear |
| rethinking / ch09_m9_4 | 0.977 [0.969–0.984] | 1.41 [1.38–1.55] | 1.45 | met | 2.54 | review/review |
| rethinking / ch09_m9_5 | 0.259 [0.257–0.274] | 0.365 [0.346–0.570] | 1.41 | met | 2.57 | clear/clear |
| rethinking / ch09_mp | 0.022 [0.014–0.024] | 0.067 [0.020–0.232] | 3.12 | met | 2.31 | clear/review |
| rethinking / ch11_m11_10 | 0.021 [0.020–0.022] | 0.029 [0.026–0.226] | 1.36 | met | 3.24 | clear/clear |
| rethinking / ch11_m11_11 | 0.135 [0.130–0.141] | 0.219 [0.207–0.424] | 1.62 | met | 3.57 | review/clear |
| rethinking / ch11_m11_4 | 0.225 [0.219–0.232] | 0.407 [0.402–0.637] | 1.81 | met | 3.02 | clear/clear |
| rethinking / ch11_m11_5 | 0.312 [0.304–0.318] | 0.659 [0.629–0.839] | 2.11 | met | 3.09 | clear/clear |
| rethinking / ch11_m11_6 | 0.038 [0.035–0.039] | 0.053 [0.050–0.250] | 1.42 | met | 3.02 | clear/clear |
| rethinking / ch11_m11_7 | 0.014 [0.014–0.015] | 0.018 [0.018–0.221] | 1.27 | met | 2.86 | clear/clear |
| rethinking / ch11_m11_8 | 0.051 [0.049–0.055] | 0.078 [0.070–0.291] | 1.51 | met | 2.94 | review/review |
| rethinking / ch11_m11_9 | 0.012 [0.011–0.017] | 0.015 [0.013–0.203] | 1.26 | met | 2.60 | clear/clear |
| rethinking / ch11_m_pois | 0.013 [0.012–0.013] | 0.014 [0.014–0.208] | 1.14 | met | 2.53 | clear/clear |
| rethinking / ch12_m12_1 | 0.031 [0.029–0.035] | 0.039 [0.038–0.225] | 1.28 | met | 3.19 | clear/clear |
| rethinking / ch12_m12_2 | 0.107 [0.101–0.116] | 0.151 [0.134–0.338] | 1.41 | met | 3.98 | clear/clear |
| rethinking / ch12_m12_3 | 0.027 [0.027–0.029] | 0.253 [0.236–0.485] | 9.28 | met | 2.50 | clear/clear |
| rethinking / ch12_m12_3_alt | 0.056 [0.050–0.062] | 0.247 [0.229–0.467] | 4.42 | met | 2.50 | clear/clear |
| rethinking / ch12_m12_4 | 0.586 [0.558–0.600] | 86.06 [80.32–89.47] | 146.9 | met | 3.19 | clear/clear |
| rethinking / ch12_m12_5 | 102.4 [95.85–103.1] | 161.4 [157.5–166.1] | 1.58 | met | 3.57 | clear/clear |
| rethinking / ch12_m12_6 | 332.3 [312.6–367.3] | 482.5 [452.7–506.6] | 1.45 | met | 4.29 | clear/clear |
| rethinking / ch12_m12_7 | 132.9 [108.9–140.5] | 213.2 [190.1–227.5] | 1.60 | met | 3.51 | clear/clear |
| rethinking / ch13_m13_1 | 0.050 [0.047–0.056] | 0.075 [0.069–0.288] | 1.49 | met | 2.98 | clear/clear |
| rethinking / ch13_m13_2 | 0.059 [0.056–0.061] | 0.087 [0.082–0.284] | 1.49 | met | 3.17 | clear/clear |
| rethinking / ch13_m13_3 | 0.073 [0.061–0.079] | 0.082 [0.076–0.289] | 1.12 | met | 3.01 | clear/clear |
| rethinking / ch13_m13_4 | 0.490 [0.423–0.616] | 0.789 [0.689–1.09] | 1.61 | met | 3.49 | review/review |
| rethinking / ch13_m13_4b | 0.421 [0.355–0.566] | 0.620 [0.542–0.929] | 1.47 | met | 3.35 | review/review |
| rethinking / ch13_m13_4nc | 0.781 [0.763–0.817] | 2.04 [2.00–2.15] | 2.62 | met | 3.33 | clear/clear |
| rethinking / ch13_m13_5 | 0.260 [0.249–0.264] | 0.467 [0.442–0.662] | 1.79 | met | 3.56 | clear/clear |
| rethinking / ch13_m13_6 | 0.793 [0.669–3.22] | 1.74 [1.67–2.05] | 2.19 | met | 4.89 | review/review |
| rethinking / ch13_m13_7 | 0.028 [0.021–0.035] | 0.037 [0.032–0.315] | 1.32 | met | 3.81 | review/review |
| rethinking / ch13_m13_7nc | 0.019 [0.017–0.023] | 0.024 [0.023–0.208] | 1.28 | met | 3.83 | clear/clear |
| rethinking / ch14_m14_1 | 0.367 [0.356–0.381] | 0.514 [0.480–0.757] | 1.40 | met | 10.26 | clear/clear |
| rethinking / ch14_m14_10 | 6.42 [5.82–6.58] | 7.99 [7.58–8.13] | 1.24 | met | 6.24 | clear/clear |
| rethinking / ch14_m14_11 | 11.21 [11.11–11.40] | 13.09 [12.66–13.24] | 1.17 | met | 6.40 | clear/clear |
| rethinking / ch14_m14_2 | 3.22 [2.73–3.92] | 4.72 [4.29–5.60] | 1.46 | met | 11.06 | review/review |
| rethinking / ch14_m14_3 | 1.28 [1.26–1.33] | 2.26 [2.17–2.48] | 1.76 | met | 9.23 | clear/clear |
| rethinking / ch14_m14_4 | 0.046 [0.043–0.059] | 0.119 [0.099–0.290] | 2.60 | met | 4.54 | clear/clear |
| rethinking / ch14_m14_4x | 0.037 [0.034–0.038] | 0.096 [0.082–0.265] | 2.61 | met | 3.76 | clear/clear |
| rethinking / ch14_m14_5 | 0.051 [0.050–0.053] | 0.170 [0.167–0.341] | 3.31 | met | 3.16 | clear/clear |
| rethinking / ch14_m14_6 | 2.44 [2.36–2.56] | 4.17 [3.89–4.45] | 1.71 | met | 6.93 | clear/clear |
| rethinking / ch14_m14_6x | 2.38 [2.29–2.46] | 4.14 [3.92–4.26] | 1.74 | met | 6.86 | clear/clear |
| rethinking / ch14_m14_7 | 4.68 [4.57–4.77] | 6.77 [6.69–6.87] | 1.45 | met | 8.96 | clear/clear |
| rethinking / ch14_m14_8 | 0.710 [0.681–0.758] | 1.07 [0.980–1.12] | 1.51 | met | 4.51 | clear/clear |
| rethinking / ch14_m14_8nc | 0.845 [0.770–1.04] | 1.21 [1.16–1.53] | 1.44 | met | 5.72 | clear/clear |
| rethinking / ch14_m14_9 | 5.04 [4.94–5.52] | 6.14 [5.76–6.58] | 1.22 | met | 4.05 | clear/clear |
| rethinking / ch15_m15_1 | 0.052 [0.050–0.055] | 0.100 [0.093–0.286] | 1.94 | met | 3.20 | clear/clear |
| rethinking / ch15_m15_2 | 0.081 [0.077–0.084] | 0.144 [0.141–0.339] | 1.79 | met | 3.43 | clear/clear |
| rethinking / ch15_m15_3 | 0.257 [0.241–0.261] | 0.296 [0.287–0.502] | 1.15 | met | 2.65 | clear/clear |
| rethinking / ch15_m15_4 | 0.208 [0.202–0.214] | 0.274 [0.263–0.429] | 1.32 | met | 2.64 | clear/clear |
| rethinking / ch15_m15_5 | 0.036 [0.032–0.041] | 0.063 [0.058–0.277] | 1.76 | met | 3.24 | clear/clear |
| rethinking / ch15_m15_6 | 0.024 [0.024–0.026] | 0.035 [0.033–0.239] | 1.44 | met | 3.00 | clear/clear |
| rethinking / ch15_m15_7 | 0.338 [0.311–0.367] | 0.558 [0.530–0.761] | 1.65 | met | 7.19 | clear/clear |
| rethinking / ch15_m15_8 | 0.055 [0.052–0.057] | 0.110 [0.105–0.299] | 2.00 | met | 2.84 | clear/clear |
| rethinking / ch15_m15_9 | 0.077 [0.075–0.081] | 0.165 [0.146–0.353] | 2.15 | met | 3.22 | clear/clear |
| rethinking / ch16_m16_1 | 3.68 [3.59–3.75] | 8.64 [8.51–8.80] | 2.35 | met | 3.01 | clear/clear |
| rethinking / ch16_m16_4 | 0.084 [0.075–0.091] | 0.239 [0.208–0.432] | 2.83 | met | 2.91 | clear/clear |
| rethinking / extra_hurdle_poisson | 0.014 [0.014–0.015] | 0.019 [0.018–0.211] | 1.29 | met | 2.61 | clear/clear |
| brms / i319_gauss_re | 0.235 [0.221–0.252] | 0.300 [0.262–0.479] | 1.28 | met | 4.58 | clear/clear |
| brms / i319_negbin_fixed | 0.196 [0.185–0.207] | 0.196 [0.136–0.446] | 1.00 | met | 3.37 | clear/review |
| brms / i319_negbin_re | 0.880 [0.796–0.895] | 1.04 [1.03–1.21] | 1.19 | met | 4.75 | clear/clear |
| brms / i319_pois_fixed | 0.098 [0.093–0.104] | 0.100 [0.098–0.295] | 1.02 | met | 3.11 | clear/clear |
| brms / i319_pois_re | 0.978 [0.956–1.09] | 1.29 [1.24–1.50] | 1.32 | met | 4.50 | clear/review |
| brms / i319_pois_re2 | 1.75 [1.68–1.79] | 2.52 [2.39–2.61] | 1.45 | met | 4.83 | clear/clear |
| brms / i320_gp_expquad | not measured | not measured | — | unmeasured | — | incomplete/incomplete |
| brms / i320_gp_matern32 | 1.70 [1.35–1.88] | 1.84 [1.31–2.02] | 1.08 | met | 6.98 | review/review |
| brms / i320_mi_nhanes | 0.710 [0.690–0.736] | 0.924 [0.878–1.05] | 1.30 | met | 4.42 | clear/review |
| brms / i320_pois_trunc_both | 0.789 [0.776–0.813] | 0.812 [0.776–0.999] | 1.03 | met | 3.60 | clear/clear |
| brms / i320_pois_trunc_ub | 0.621 [0.607–0.632] | 0.659 [0.635–0.825] | 1.06 | met | 3.50 | clear/clear |
| brms / i320_sratio_cs | 1.80 [1.72–1.82] | 2.73 [2.54–3.00] | 1.52 | met | 5.05 | clear/clear |
| brms / i320_sratio_plain | 1.05 [1.000–1.24] | 1.50 [1.48–1.74] | 1.44 | met | 3.78 | clear/clear |
| brms / s2_ar_cov | 0.106 [0.100–0.113] | 0.120 [0.110–0.342] | 1.13 | met | 7.23 | clear/clear |
| brms / s2_beta_binomial | 0.069 [0.063–0.074] | 0.084 [0.079–0.276] | 1.23 | met | 3.70 | clear/clear |
| brms / s2_car | 0.065 [0.059–0.076] | 0.126 [0.103–0.301] | 1.94 | met | 4.38 | review/review |
| brms / s2_car_esicar | 0.059 [0.053–0.063] | 0.089 [0.079–0.288] | 1.50 | met | 4.50 | review/review |
| brms / s2_car_icar | 2.04 [1.98–2.05] | 4.30 [4.17–4.50] | 2.11 | met | 4.39 | review/review |
| brms / s2_categorical_re | 0.230 [0.224–0.233] | 0.361 [0.315–0.530] | 1.57 | met | 5.44 | clear/review |
| brms / s2_cens_interval | 0.034 [0.032–0.038] | 0.045 [0.044–0.255] | 1.32 | met | 3.77 | clear/clear |
| brms / s2_com_poisson | not measured | not measured | — | unmeasured | — | incomplete/incomplete |
| brms / s2_cosy | 0.136 [0.128–0.144] | 0.148 [0.139–0.358] | 1.09 | met | 6.54 | clear/clear |
| brms / s2_cox | 0.048 [0.045–0.051] | 0.108 [0.101–0.328] | 2.25 | met | 4.16 | clear/clear |
| brms / s2_cox_cens | 0.045 [0.043–0.046] | 0.123 [0.099–0.327] | 2.71 | met | 5.06 | clear/clear |
| brms / s2_cumulative_cauchit | 0.046 [0.043–0.050] | 0.117 [0.106–0.322] | 2.58 | met | 3.67 | clear/clear |
| brms / s2_cumulative_cloglog | 0.048 [0.043–0.053] | 0.098 [0.085–0.300] | 2.05 | met | 3.67 | clear/clear |
| brms / s2_cumulative_probit | 0.089 [0.081–0.090] | 0.116 [0.104–0.302] | 1.30 | met | 3.71 | clear/clear |
| brms / s2_custom_vint | 0.068 [0.066–0.071] | 0.089 [0.083–0.280] | 1.30 | met | 3.54 | clear/clear |
| brms / s2_custom_vreal | 0.021 [0.019–0.024] | 0.041 [0.038–0.259] | 1.94 | met | 3.46 | clear/clear |
| brms / s2_dirichlet | 0.242 [0.237–0.247] | 0.272 [0.266–0.451] | 1.12 | met | 4.37 | clear/clear |
| brms / s2_discrete_weibull | 0.087 [0.080–0.094] | 0.172 [0.158–0.349] | 1.98 | met | 3.49 | clear/clear |
| brms / s2_dist_sigma_re | 0.069 [0.061–0.079] | 0.116 [0.109–0.323] | 1.69 | met | 4.94 | review/review |
| brms / s2_fcor | 0.179 [0.162–0.194] | 0.183 [0.169–0.393] | 1.02 | met | 4.94 | clear/clear |
| brms / s2_frechet | 0.093 [0.078–0.095] | 0.138 [0.134–0.332] | 1.49 | met | 3.97 | clear/clear |
| brms / s2_gev | 0.225 [0.219–0.247] | 0.095 [0.090–0.292] | 0.420 | below | 3.79 | clear/clear |
| brms / s2_gp_approx | 0.058 [0.050–0.062] | 0.137 [0.118–0.332] | 2.37 | met | 5.25 | review/review |
| brms / s2_gp_by_approx | 0.081 [0.073–0.091] | 0.229 [0.205–0.393] | 2.81 | met | 6.02 | review/review |
| brms / s2_gp_by_gr | not measured | not measured | — | unmeasured | — | incomplete/incomplete |
| brms / s2_gr_by | 0.077 [0.058–0.087] | 0.100 [0.088–0.316] | 1.30 | met | 4.93 | review/review |
| brms / s2_gr_student | 0.112 [0.100–0.122] | 0.169 [0.161–0.353] | 1.51 | met | 5.18 | review/review |
| brms / s2_hurdle_cumulative | 0.116 [0.112–0.124] | 0.179 [0.170–0.374] | 1.53 | met | 4.19 | clear/clear |
| brms / s2_hurdle_negbin | 0.089 [0.080–0.099] | 0.151 [0.144–0.372] | 1.70 | met | 3.49 | review/review |
| brms / s2_index_mi | 0.051 [0.046–0.054] | 0.062 [0.056–0.265] | 1.24 | met | 4.22 | clear/clear |
| brms / s2_invgaussian | not measured | not measured | — | unmeasured | — | incomplete/incomplete |
| brms / s2_logistic_normal | 0.341 [0.336–0.349] | 0.468 [0.462–0.673] | 1.37 | met | 6.35 | clear/clear |
| brms / s2_me2 | 0.310 [0.300–0.324] | 0.443 [0.383–0.662] | 1.43 | met | 6.73 | clear/review |
| brms / s2_me2_nomecor | 0.184 [0.171–0.189] | 0.320 [0.306–0.508] | 1.74 | met | 4.43 | review/review |
| brms / s2_mi_lognormal | 0.036 [0.034–0.039] | 0.055 [0.052–0.266] | 1.56 | met | 4.60 | clear/clear |
| brms / s2_mi_trunc_lb | 0.199 [0.177–0.226] | 0.268 [0.211–0.426] | 1.35 | met | 4.46 | review/review |
| brms / s2_mixture_theta | 2.11 [1.51–2.50] | 1.62 [0.766–3.70] | 0.771 | below | 4.55 | review/review |
| brms / s2_mm | 0.076 [0.073–0.080] | 0.090 [0.077–0.276] | 1.18 | met | 4.67 | review/review |
| brms / s2_mm_weights | 0.077 [0.070–0.091] | 0.082 [0.077–0.280] | 1.06 | met | 4.66 | review/review |
| brms / s2_mmc | 0.183 [0.173–0.191] | 0.223 [0.191–0.400] | 1.22 | met | 6.97 | review/review |
| brms / s2_mo_simo_prior | 0.048 [0.045–0.050] | 0.074 [0.070–0.254] | 1.53 | met | 4.01 | clear/clear |
| brms / s2_multinomial | 0.163 [0.156–0.173] | 0.216 [0.204–0.419] | 1.32 | met | 4.01 | clear/clear |
| brms / s2_mv_shared_re | 0.154 [0.141–0.168] | 0.191 [0.163–0.377] | 1.24 | met | 7.15 | review/review |
| brms / s2_mv_subset | 0.028 [0.027–0.031] | 0.031 [0.028–0.218] | 1.09 | met | 3.48 | clear/clear |
| brms / s2_nl_noloop | 0.064 [0.052–0.068] | 0.156 [0.119–0.383] | 2.45 | met | 3.92 | review/review |
| brms / s2_nlf | 0.028 [0.027–0.028] | 0.053 [0.051–0.262] | 1.92 | met | 3.98 | clear/clear |
| brms / s2_rate | 0.019 [0.019–0.020] | 0.028 [0.027–0.235] | 1.48 | met | 3.61 | clear/clear |
| brms / s2_s_by | 0.177 [0.171–0.237] | 0.492 [0.481–0.727] | 2.78 | met | 5.91 | review/review |
| brms / s2_s_cc | 0.039 [0.036–0.042] | 0.077 [0.075–0.258] | 1.99 | met | 4.32 | review/review |
| brms / s2_sar | 0.063 [0.061–0.065] | 0.138 [0.128–0.331] | 2.20 | met | 4.07 | clear/clear |
| brms / s2_sar_error | 0.067 [0.063–0.070] | 0.141 [0.122–0.345] | 2.12 | met | 4.13 | clear/clear |
| brms / s2_shifted_lognormal | 0.025 [0.024–0.029] | 0.042 [0.041–0.233] | 1.67 | met | 3.93 | clear/clear |
| brms / s2_t2_by | 0.115 [0.112–0.122] | 0.283 [0.272–0.453] | 2.46 | met | 5.87 | review/review |
| brms / s2_threading | 0.021 [0.019–0.022] | 0.023 [0.022–0.209] | 1.14 | met | 3.37 | clear/clear |
| brms / s2_unstr | 0.461 [0.454–0.486] | 0.521 [0.465–0.701] | 1.13 | met | 7.56 | review/review |
| brms / s2_weights_trunc | 0.205 [0.174–0.236] | 0.236 [0.209–0.425] | 1.15 | met | 3.62 | review/review |
| brms / s2_wiener | 0.453 [0.442–0.481] | 0.472 [0.428–0.659] | 1.04 | met | 3.66 | clear/clear |
| brms / s2_zi_asymlaplace | 0.212 [0.204–0.219] | 0.148 [0.136–0.325] | 0.700 | below | 3.58 | clear/clear |
| brms / s2_zi_beta | 0.069 [0.066–0.076] | 0.113 [0.100–0.308] | 1.63 | met | 3.63 | clear/clear |
| brms / s2_zoi_beta | 0.080 [0.076–0.083] | 0.125 [0.112–0.335] | 1.56 | met | 3.69 | clear/clear |
| brms / sw_acat | 0.718 [0.699–0.742] | 3.17 [3.10–3.23] | 4.42 | met | 3.90 | clear/clear |
| brms / sw_acat_cs | 1.33 [1.23–1.38] | 5.32 [5.13–5.53] | 3.99 | met | 5.03 | clear/clear |
| brms / sw_ar | 0.041 [0.038–0.043] | 0.056 [0.051–0.252] | 1.36 | met | 4.26 | clear/clear |
| brms / sw_arma | 0.143 [0.115–0.162] | 0.208 [0.172–0.438] | 1.45 | met | 4.35 | review/review |
| brms / sw_asymlaplace | 0.175 [0.171–0.183] | 0.118 [0.107–0.289] | 0.677 | below | 3.74 | clear/clear |
| brms / sw_bernoulli | 0.020 [0.018–0.020] | 0.022 [0.022–0.212] | 1.14 | met | 3.28 | clear/clear |
| brms / sw_beta | 0.037 [0.034–0.040] | 0.056 [0.049–0.249] | 1.49 | met | 3.89 | clear/clear |
| brms / sw_binomial | 0.031 [0.028–0.033] | 0.038 [0.036–0.257] | 1.26 | met | 3.54 | clear/clear |
| brms / sw_categorical | 0.036 [0.034–0.038] | 0.038 [0.037–0.250] | 1.06 | met | 4.32 | clear/clear |
| brms / sw_cens | 0.028 [0.027–0.031] | 0.043 [0.043–0.245] | 1.52 | met | 4.14 | clear/clear |
| brms / sw_cratio | 0.763 [0.730–0.779] | 1.07 [1.02–1.28] | 1.40 | met | 3.62 | clear/clear |
| brms / sw_cratio_cs | 1.28 [1.20–1.52] | 2.08 [1.95–2.20] | 1.63 | met | 4.73 | clear/clear |
| brms / sw_cumulative | 0.315 [0.306–0.336] | 0.302 [0.292–0.485] | 0.957 | met | 3.78 | clear/clear |
| brms / sw_cumulative_cs | 1.38 [1.22–1.61] | 1.51 [1.28–1.80] | 1.09 | met | 4.77 | review/review |
| brms / sw_dist_sigma | 0.026 [0.025–0.027] | 0.049 [0.046–0.256] | 1.85 | met | 3.85 | clear/clear |
| brms / sw_exgaussian | 0.054 [0.048–0.059] | 0.087 [0.080–0.285] | 1.60 | met | 3.90 | review/review |
| brms / sw_gamma | 0.025 [0.023–0.025] | 0.039 [0.036–0.235] | 1.58 | met | 3.75 | clear/clear |
| brms / sw_gaussian | 0.020 [0.019–0.021] | 0.029 [0.022–0.201] | 1.49 | met | 3.73 | clear/clear |
| brms / sw_gp | not measured | not measured | — | unmeasured | — | incomplete/incomplete |
| brms / sw_hurdle_gamma | 0.044 [0.040–0.046] | 0.074 [0.067–0.283] | 1.69 | met | 3.55 | clear/clear |
| brms / sw_hurdle_lognormal | 0.037 [0.036–0.037] | 0.061 [0.061–0.273] | 1.67 | met | 3.59 | clear/clear |
| brms / sw_hurdle_pois | 0.037 [0.035–0.041] | 0.069 [0.066–0.275] | 1.85 | met | 3.39 | clear/clear |
| brms / sw_lognormal | 0.021 [0.020–0.021] | 0.031 [0.030–0.230] | 1.49 | met | 3.70 | clear/clear |
| brms / sw_ma | 0.045 [0.042–0.046] | 0.059 [0.054–0.277] | 1.33 | met | 4.30 | clear/clear |
| brms / sw_me | 0.099 [0.096–0.117] | 0.187 [0.164–0.368] | 1.89 | met | 4.20 | review/review |
| brms / sw_mi | 0.039 [0.038–0.039] | 0.041 [0.037–0.241] | 1.06 | met | 3.83 | clear/clear |
| brms / sw_mixture | 1.74 [1.64–1.90] | 2.57 [2.39–2.65] | 1.47 | met | 4.79 | review/review |
| brms / sw_mono | 0.045 [0.040–0.046] | 0.067 [0.060–0.257] | 1.48 | met | 4.00 | clear/clear |
| brms / sw_mv_norescor | 0.028 [0.027–0.033] | 0.030 [0.028–0.228] | 1.07 | met | 3.47 | clear/clear |
| brms / sw_mv_rescor | 0.137 [0.127–0.140] | 0.167 [0.158–0.397] | 1.22 | met | 6.23 | clear/clear |
| brms / sw_negbinomial | 2.62 [2.08–2.84] | 3.23 [3.09–3.84] | 1.23 | met | 3.39 | review/review |
| brms / sw_nonlinear | 0.023 [0.022–0.025] | 0.046 [0.044–0.251] | 1.96 | met | 3.87 | clear/clear |
| brms / sw_poisson | 0.022 [0.022–0.023] | 0.025 [0.024–0.224] | 1.11 | met | 3.13 | clear/clear |
| brms / sw_re_bern | 0.050 [0.048–0.053] | 0.078 [0.071–0.284] | 1.56 | met | 4.65 | clear/clear |
| brms / sw_re_gauss | 0.056 [0.049–0.059] | 0.075 [0.072–0.279] | 1.34 | met | 4.59 | review/review |
| brms / sw_re_negbin | capped 1/4 | 5.01 [0.216–5.84] | — | unmeasured | 4.79 | incomplete/review |
| brms / sw_re_pois | 0.058 [0.049–0.066] | 0.086 [0.077–0.286] | 1.48 | met | 4.50 | review/review |
| brms / sw_re_slope | 0.173 [0.161–0.178] | 0.211 [0.166–0.424] | 1.23 | met | 6.82 | review/review |
| brms / sw_se | 0.018 [0.017–0.018] | 0.024 [0.024–0.221] | 1.37 | met | 3.55 | clear/clear |
| brms / sw_skewnormal | 0.048 [0.048–0.049] | 0.059 [0.054–0.246] | 1.22 | met | 3.97 | clear/clear |
| brms / sw_spline_s | 0.063 [0.058–0.066] | 0.158 [0.136–0.335] | 2.53 | met | 4.47 | review/review |
| brms / sw_spline_t2 | 0.082 [0.072–0.090] | 0.181 [0.157–0.387] | 2.21 | met | 4.93 | review/review |
| brms / sw_sratio | 0.763 [0.749–0.781] | 1.09 [1.04–1.32] | 1.43 | met | 3.63 | clear/clear |
| brms / sw_student | 0.029 [0.029–0.030] | 0.044 [0.043–0.237] | 1.53 | met | 3.94 | clear/clear |
| brms / sw_trunc | 0.212 [0.179–0.222] | 0.237 [0.184–0.452] | 1.12 | met | 3.59 | review/review |
| brms / sw_vonmises | 0.912 [0.768–1.20] | 1.72 [1.57–1.85] | 1.89 | met | 3.69 | review/review |
| brms / sw_weibull | 0.046 [0.044–0.048] | 0.062 [0.061–0.253] | 1.34 | met | 3.95 | clear/clear |
| brms / sw_weights | 0.023 [0.022–0.027] | 0.039 [0.039–0.257] | 1.70 | met | 3.53 | clear/clear |
| brms / sw_zi_binomial | 0.047 [0.043–0.049] | 0.071 [0.064–0.290] | 1.51 | met | 3.42 | clear/clear |
| brms / sw_zi_negbin | 0.078 [0.073–0.090] | 0.117 [0.102–0.319] | 1.50 | met | 3.47 | review/review |
| brms / sw_zi_poisson | 0.034 [0.030–0.037] | 0.048 [0.042–0.239] | 1.44 | met | 3.37 | clear/clear |

## Incomplete fixtures

- **i320_gp_expquad**: density/gradient mismatch: scaled error 1.18e-09
- **s2_com_poisson**: s2_com_poisson/gradient/0/stanli: failed (event 2828)
- **s2_gp_by_gr**: density/gradient mismatch: scaled error 7.33e-07
- **s2_invgaussian**: s2_invgaussian/gradient/0/stanli: failed (event 3467)
- **sw_gp**: density/gradient mismatch: scaled error 7.83e-09
- **sw_re_negbin**: stanli seed 2: timeout (limit 0.649 s)

## Reproduce the tables

Retain the original run directory, including model headers, inputs, logs and per-seed CSVs. After the timed sweep finishes, run:

```sh
python3 tools/teaching_diagnostic_jobs.py RUN_DIRECTORY /tmp/teaching-jobs.json
Rscript tools/summarize_rethinking_bench.R /tmp/teaching-jobs.json /tmp/teaching-diagnostics.json
python3 tools/report_teaching.py RUN_DIRECTORY /tmp/teaching-diagnostics.json output/teaching-performance
```

The jobs file contains absolute CSV paths; regenerate it after relocating the evidence directory. The exporter refuses an unfinished sweep. See the [benchmark method](../../docs/benchmarks.md#how-we-measure).
