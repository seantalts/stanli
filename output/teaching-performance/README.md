# Teaching model performance

Recorded platform: macOS-26.6.2-arm64-arm-64bit; 32 logical CPUs. Source checkout: `ea4d7e2470e22386705afb3be3ad0a86037da90d`. The manifest records tracked differences, configuration, dependencies, and executable hashes.

Run `a224d12afe02ac98` includes every one of its 199 fixtures. Failures and timeouts remain in the tables.

## Collection summary

| Collection | Fixtures | Completed in both | Lower Stanli CLI time | Median CmdStan/Stanli CLI ratio | Screen clear in both |
| --- | ---: | ---: | ---: | ---: | ---: |
| educational | 13 | 13 | 12/13 | 1.30 | 10/13 |
| rethinking | 62 | 61 | 58/61 | 1.52 | 50/61 |
| brms | 124 | 118 | 108/118 | 1.45 | 76/118 |

A ratio above one means less elapsed time for Stanli. Each model has equal weight in the median; these ratios describe the completed fixtures only. Diagnostic flags are not removed from the timing summary.

## What was measured

Each engine ran 4 independent single-chain seeds, with 1000 warmup iterations and 1000 retained draws per seed, target acceptance 0.8, tree depth 10, and random initialization. The table reports median [minimum–maximum] CLI seconds. Stanli includes model preparation; CmdStan starts from a compiled model. Both include generated quantities and CSV output. Toolchain installation is excluded.

CSV precision follows the recorded CLI defaults. The numerical oracle uses separate high-precision values; sampling diagnostics use these retained CSVs.

CmdStan compilation is measured once, including Stan translation and the C++ model build. Adding it to the median CmdStan CLI time gives an estimated first fit; this is a sum of measured stages. It is not a directly timed four-chain R fit or a cold-cache measurement.

CmdStan ran first for each seed. Stanli's cap was min(3 × that CmdStan CLI time, 900 seconds). A failed, invalid, or capped seed prevents an aggregate for that engine. A preceding preparation or gradient-gate failure leaves sampling unmeasured. No partial-seed averages are substituted.

Screen clear: no retained-draw divergences or depth hits, finite R-hat ≤ 1.01 and bulk ESS ≥ 400 for every nonconstant variable in the parameters block. Structural fixed matrix entries are checked and omitted. The CSV also records tail ESS, minimum E-BFMI, and minimum bulk ESS divided by the sum of the four serial CLI durations. These are descriptive diagnostics; fixed-budget runtime is not time to equal inferential accuracy.

Gradient results in the CSV use six alternating pairs at the same parameter point. Each pair must satisfy the full density/gradient scaled-error gate of 1e-9. The separate three-point numerical replay and its known exceptions are described in the [support guide](../../docs/teaching-support.md).

## Full appendix

| Collection / fixture | Stanli CLI | CmdStan CLI | CmdStan compile | Screen S/C |
| --- | ---: | ---: | ---: | --- |
| educational / aalto_bern | 0.012 [0.011–0.013] | 0.014 [0.013–0.192] | 2.55 | clear/clear |
| educational / aalto_binom | 0.011 [0.011–0.012] | 0.013 [0.012–0.205] | 2.37 | clear/clear |
| educational / aalto_binom2 | 0.013 [0.013–0.014] | 0.015 [0.015–0.218] | 2.48 | clear/clear |
| educational / aalto_binomb | 0.011 [0.011–0.011] | 0.012 [0.012–0.197] | 2.39 | clear/clear |
| educational / aalto_gpareto | 0.028 [0.026–0.029] | 0.025 [0.024–0.225] | 2.90 | clear/clear |
| educational / aalto_grp_aov | 0.018 [0.017–0.022] | 0.025 [0.024–0.232] | 3.08 | clear/clear |
| educational / aalto_grp_prior_mean | 0.028 [0.026–0.032] | 0.037 [0.031–0.251] | 3.16 | review/review |
| educational / aalto_grp_prior_mean_var | 0.061 [0.054–0.068] | 0.104 [0.089–0.297] | 3.92 | review/review |
| educational / aalto_lin | 0.021 [0.021–0.022] | 0.028 [0.027–0.218] | 3.07 | clear/review |
| educational / aalto_lin_std | 0.020 [0.019–0.020] | 0.026 [0.025–0.228] | 3.30 | clear/clear |
| educational / aalto_lin_std_t | 0.021 [0.021–0.023] | 0.028 [0.027–0.216] | 3.50 | clear/clear |
| educational / aalto_poisson_hurdle | 0.648 [0.641–0.659] | 0.824 [0.791–1.04] | 2.92 | clear/clear |
| educational / aalto_poisson_simple | 0.097 [0.093–0.098] | 0.154 [0.152–0.344] | 2.65 | clear/clear |
| rethinking / ch09_m5_8s | 1.22 [1.20–1.26] | 3.54 [3.42–3.68] | 2.86 | review/review |
| rethinking / ch09_m5_8s2 | 0.860 [0.301–1.17] | 2.59 [1.12–3.32] | 2.86 | review/review |
| rethinking / ch09_m9_1 | 0.030 [0.026–0.033] | 0.057 [0.052–0.272] | 3.09 | clear/clear |
| rethinking / ch09_m9_1_chains4 | 0.028 [0.025–0.032] | 0.052 [0.046–0.231] | 3.06 | clear/clear |
| rethinking / ch09_m9_2 | 0.017 [0.017–0.019] | 0.025 [0.022–0.224] | 2.53 | review/review |
| rethinking / ch09_m9_3 | 0.013 [0.012–0.019] | 0.018 [0.015–0.212] | 2.52 | clear/clear |
| rethinking / ch09_m9_4 | 0.978 [0.964–0.990] | 1.43 [1.36–1.58] | 2.55 | review/review |
| rethinking / ch09_m9_5 | 0.268 [0.259–0.277] | 0.353 [0.336–0.571] | 2.56 | clear/clear |
| rethinking / ch09_mp | 0.022 [0.014–0.027] | 0.068 [0.020–0.239] | 2.32 | clear/review |
| rethinking / ch11_m11_10 | 0.019 [0.019–0.024] | 0.026 [0.026–0.244] | 3.24 | clear/clear |
| rethinking / ch11_m11_11 | 0.137 [0.133–0.139] | 0.214 [0.207–0.401] | 3.56 | review/clear |
| rethinking / ch11_m11_4 | 0.213 [0.207–0.223] | 0.397 [0.387–0.586] | 3.01 | clear/clear |
| rethinking / ch11_m11_5 | 0.308 [0.296–0.320] | 0.652 [0.623–0.857] | 3.10 | clear/clear |
| rethinking / ch11_m11_6 | 0.037 [0.035–0.039] | 0.054 [0.049–0.270] | 3.04 | clear/clear |
| rethinking / ch11_m11_7 | 0.015 [0.014–0.015] | 0.019 [0.018–0.211] | 2.85 | clear/clear |
| rethinking / ch11_m11_8 | 0.051 [0.051–0.052] | 0.078 [0.069–0.275] | 2.94 | review/review |
| rethinking / ch11_m11_9 | 0.012 [0.011–0.016] | 0.015 [0.013–0.192] | 2.60 | clear/clear |
| rethinking / ch11_m_pois | 0.013 [0.012–0.013] | 0.014 [0.014–0.207] | 2.53 | clear/clear |
| rethinking / ch12_m12_1 | 0.031 [0.028–0.033] | 0.042 [0.038–0.242] | 3.21 | clear/clear |
| rethinking / ch12_m12_2 | 0.106 [0.096–0.122] | 0.150 [0.134–0.366] | 3.97 | clear/clear |
| rethinking / ch12_m12_3 | 0.028 [0.027–0.030] | 0.260 [0.240–0.428] | 2.48 | clear/clear |
| rethinking / ch12_m12_3_alt | 0.057 [0.050–0.061] | 0.245 [0.239–0.448] | 2.50 | clear/clear |
| rethinking / ch12_m12_4 | 0.598 [0.578–0.603] | 86.31 [80.06–90.49] | 3.17 | clear/clear |
| rethinking / ch12_m12_5 | 101.2 [99.56–102.0] | 161.8 [157.8–165.4] | 3.58 | clear/clear |
| rethinking / ch12_m12_6 | 326.5 [316.4–371.2] | 484.4 [455.8–509.2] | 4.27 | clear/clear |
| rethinking / ch12_m12_7 | 131.0 [108.0–137.8] | 213.5 [189.7–226.1] | 3.55 | clear/clear |
| rethinking / ch13_m13_1 | 0.049 [0.047–0.053] | 0.075 [0.068–0.263] | 2.95 | clear/clear |
| rethinking / ch13_m13_2 | 0.057 [0.054–0.061] | 0.088 [0.086–0.286] | 3.14 | clear/clear |
| rethinking / ch13_m13_3 | 0.068 [0.062–0.071] | 0.082 [0.074–0.280] | 3.03 | clear/clear |
| rethinking / ch13_m13_4 | 0.496 [0.418–0.631] | 0.788 [0.681–1.11] | 3.41 | review/review |
| rethinking / ch13_m13_4b | 0.421 [0.325–0.544] | 0.601 [0.506–0.915] | 3.30 | review/review |
| rethinking / ch13_m13_4nc | 0.746 [0.714–0.771] | 1.90 [1.87–2.10] | 3.31 | clear/clear |
| rethinking / ch13_m13_5 | 0.231 [0.225–0.243] | 0.429 [0.426–0.614] | 3.27 | clear/clear |
| rethinking / ch13_m13_6 | capped 1/4 | 1.01 [0.858–1.18] | 3.39 | incomplete/review |
| rethinking / ch13_m13_7 | 0.015 [0.013–0.017] | 0.024 [0.018–0.203] | 2.30 | review/review |
| rethinking / ch13_m13_7nc | 0.012 [0.010–0.016] | 0.015 [0.013–0.195] | 2.35 | clear/clear |
| rethinking / ch14_m14_1 | 0.307 [0.295–0.329] | 0.413 [0.388–0.700] | 6.95 | clear/clear |
| rethinking / ch14_m14_10 | 6.17 [5.69–6.32] | 6.19 [6.03–6.31] | 4.08 | clear/clear |
| rethinking / ch14_m14_11 | 22.70 [22.68–22.84] | 10.18 [9.58–10.41] | 4.11 | clear/clear |
| rethinking / ch14_m14_2 | 2.60 [2.22–3.14] | 3.71 [3.49–4.43] | 7.09 | review/review |
| rethinking / ch14_m14_3 | 0.979 [0.968–1.02] | 1.74 [1.74–1.94] | 5.92 | clear/clear |
| rethinking / ch14_m14_4 | 0.033 [0.031–0.036] | 0.082 [0.078–0.279] | 2.80 | clear/clear |
| rethinking / ch14_m14_4x | 0.032 [0.031–0.033] | 0.077 [0.075–0.261] | 2.79 | clear/clear |
| rethinking / ch14_m14_5 | 0.049 [0.046–0.053] | 0.167 [0.157–0.372] | 2.83 | clear/clear |
| rethinking / ch14_m14_6 | 2.76 [2.62–2.86] | 3.98 [3.77–4.08] | 6.64 | clear/clear |
| rethinking / ch14_m14_6x | 2.78 [2.67–2.87] | 3.99 [3.86–4.10] | 6.66 | clear/clear |
| rethinking / ch14_m14_7 | 4.65 [4.51–4.70] | 6.53 [6.44–6.71] | 8.88 | clear/clear |
| rethinking / ch14_m14_8 | 0.837 [0.806–0.906] | 1.04 [0.944–1.06] | 4.50 | clear/clear |
| rethinking / ch14_m14_8nc | 0.795 [0.726–1.02] | 1.19 [1.12–1.51] | 5.72 | clear/clear |
| rethinking / ch14_m14_9 | 6.23 [6.11–6.78] | 6.16 [5.80–6.55] | 4.05 | clear/clear |
| rethinking / ch15_m15_1 | 0.050 [0.049–0.052] | 0.101 [0.091–0.287] | 3.18 | clear/clear |
| rethinking / ch15_m15_2 | 0.080 [0.075–0.084] | 0.147 [0.134–0.343] | 3.41 | clear/clear |
| rethinking / ch15_m15_3 | 0.258 [0.242–0.265] | 0.291 [0.279–0.503] | 2.64 | clear/clear |
| rethinking / ch15_m15_4 | 0.216 [0.203–0.221] | 0.261 [0.243–0.442] | 2.63 | clear/clear |
| rethinking / ch15_m15_5 | 0.037 [0.032–0.039] | 0.061 [0.057–0.245] | 3.23 | clear/clear |
| rethinking / ch15_m15_6 | 0.025 [0.024–0.027] | 0.035 [0.034–0.245] | 2.98 | clear/clear |
| rethinking / ch15_m15_7 | 0.579 [0.534–0.625] | 0.542 [0.532–0.726] | 7.19 | clear/clear |
| rethinking / ch15_m15_8 | 0.054 [0.051–0.057] | 0.109 [0.106–0.318] | 2.86 | clear/clear |
| rethinking / ch15_m15_9 | 0.079 [0.072–0.081] | 0.159 [0.146–0.349] | 3.00 | clear/clear |
| rethinking / ch16_m16_1 | 3.62 [3.56–3.69] | 8.58 [8.54–8.69] | 2.99 | clear/clear |
| rethinking / ch16_m16_4 | 0.079 [0.073–0.090] | 0.239 [0.208–0.427] | 2.91 | clear/clear |
| rethinking / extra_hurdle_poisson | 0.014 [0.013–0.015] | 0.019 [0.017–0.219] | 2.60 | clear/clear |
| brms / i319_gauss_re | 0.227 [0.203–0.251] | 0.288 [0.255–0.483] | 4.61 | clear/clear |
| brms / i319_negbin_fixed | 0.201 [0.177–0.206] | 0.191 [0.132–0.424] | 3.37 | clear/review |
| brms / i319_negbin_re | 0.861 [0.788–0.882] | 1.04 [1.02–1.17] | 4.74 | clear/clear |
| brms / i319_pois_fixed | 0.096 [0.088–0.101] | 0.101 [0.094–0.311] | 3.10 | clear/clear |
| brms / i319_pois_re | 0.978 [0.970–1.08] | 1.26 [1.23–1.48] | 4.52 | clear/review |
| brms / i319_pois_re2 | 1.75 [1.68–1.78] | 2.54 [2.39–2.60] | 4.82 | clear/clear |
| brms / i320_gp_expquad | not measured | not measured | — | incomplete/incomplete |
| brms / i320_gp_matern32 | 1.68 [1.38–1.98] | 1.84 [1.31–2.03] | 6.97 | review/review |
| brms / i320_mi_nhanes | 0.705 [0.690–0.742] | 0.906 [0.863–1.03] | 4.40 | clear/review |
| brms / i320_pois_trunc_both | 1.09 [1.09–1.19] | 0.799 [0.768–0.980] | 3.55 | clear/clear |
| brms / i320_pois_trunc_ub | 0.973 [0.939–0.987] | 0.654 [0.634–0.841] | 3.50 | clear/clear |
| brms / i320_sratio_cs | 1.80 [1.77–1.84] | 2.72 [2.55–2.96] | 5.04 | clear/clear |
| brms / i320_sratio_plain | 1.03 [0.985–1.22] | 1.51 [1.46–1.75] | 3.79 | clear/clear |
| brms / s2_ar_cov | 0.112 [0.104–0.120] | 0.121 [0.110–0.330] | 7.25 | clear/clear |
| brms / s2_beta_binomial | 0.069 [0.063–0.072] | 0.087 [0.076–0.309] | 3.71 | clear/clear |
| brms / s2_car | 0.065 [0.063–0.066] | 0.128 [0.102–0.307] | 4.39 | review/review |
| brms / s2_car_esicar | 0.058 [0.052–0.064] | 0.093 [0.078–0.272] | 4.50 | review/review |
| brms / s2_car_icar | 1.94 [1.93–1.97] | 4.28 [4.16–4.55] | 4.45 | review/review |
| brms / s2_categorical_re | 0.428 [0.414–0.456] | 0.356 [0.315–0.542] | 5.46 | clear/review |
| brms / s2_cens_interval | 0.035 [0.034–0.036] | 0.050 [0.044–0.261] | 3.79 | clear/clear |
| brms / s2_com_poisson | not measured | not measured | — | incomplete/incomplete |
| brms / s2_cosy | 0.141 [0.129–0.160] | 0.157 [0.141–0.365] | 6.51 | clear/clear |
| brms / s2_cox | 0.050 [0.048–0.053] | 0.113 [0.102–0.312] | 4.21 | clear/clear |
| brms / s2_cox_cens | 0.045 [0.043–0.048] | 0.122 [0.099–0.314] | 5.10 | clear/clear |
| brms / s2_cumulative_cauchit | 0.046 [0.042–0.048] | 0.124 [0.105–0.329] | 3.67 | clear/clear |
| brms / s2_cumulative_cloglog | 0.047 [0.045–0.049] | 0.100 [0.091–0.316] | 3.67 | clear/clear |
| brms / s2_cumulative_probit | 0.087 [0.083–0.092] | 0.114 [0.105–0.308] | 3.73 | clear/clear |
| brms / s2_custom_vint | 0.069 [0.062–0.071] | 0.090 [0.083–0.301] | 3.55 | clear/clear |
| brms / s2_custom_vreal | 0.021 [0.019–0.024] | 0.039 [0.035–0.268] | 3.48 | clear/clear |
| brms / s2_dirichlet | 0.224 [0.219–0.243] | 0.282 [0.278–0.457] | 4.40 | clear/clear |
| brms / s2_discrete_weibull | 0.087 [0.082–0.089] | 0.173 [0.154–0.388] | 3.47 | clear/clear |
| brms / s2_dist_sigma_re | 0.065 [0.063–0.077] | 0.122 [0.106–0.307] | 4.96 | review/review |
| brms / s2_fcor | 0.182 [0.168–0.190] | 0.186 [0.169–0.368] | 4.92 | clear/clear |
| brms / s2_frechet | 0.090 [0.087–0.100] | 0.142 [0.131–0.329] | 3.96 | clear/clear |
| brms / s2_gev | 0.231 [0.219–0.264] | 0.095 [0.088–0.311] | 3.80 | clear/clear |
| brms / s2_gp_approx | 0.054 [0.052–0.060] | 0.141 [0.107–0.320] | 5.22 | review/review |
| brms / s2_gp_by_approx | 0.081 [0.077–0.094] | 0.232 [0.190–0.411] | 6.03 | review/review |
| brms / s2_gp_by_gr | not measured | not measured | — | incomplete/incomplete |
| brms / s2_gr_by | 0.073 [0.059–0.081] | 0.103 [0.087–0.306] | 4.95 | review/review |
| brms / s2_gr_student | 0.112 [0.094–0.120] | 0.179 [0.162–0.351] | 5.19 | review/review |
| brms / s2_hurdle_cumulative | 0.116 [0.113–0.118] | 0.182 [0.170–0.405] | 4.15 | clear/clear |
| brms / s2_hurdle_negbin | 0.089 [0.079–0.098] | 0.150 [0.144–0.360] | 3.49 | review/review |
| brms / s2_index_mi | 0.050 [0.047–0.055] | 0.062 [0.057–0.294] | 4.20 | clear/clear |
| brms / s2_invgaussian | not measured | not measured | — | incomplete/incomplete |
| brms / s2_logistic_normal | 0.406 [0.387–0.415] | 0.468 [0.452–0.678] | 6.34 | clear/clear |
| brms / s2_me2 | 0.313 [0.297–0.328] | 0.438 [0.387–0.672] | 6.76 | clear/review |
| brms / s2_me2_nomecor | 0.183 [0.168–0.193] | 0.319 [0.300–0.501] | 4.44 | review/review |
| brms / s2_mi_lognormal | 0.036 [0.034–0.039] | 0.055 [0.051–0.259] | 4.60 | clear/clear |
| brms / s2_mi_trunc_lb | 0.204 [0.170–0.225] | 0.286 [0.204–0.402] | 4.47 | review/review |
| brms / s2_mixture_theta | 2.09 [1.51–2.49] | 1.64 [0.776–3.64] | 4.56 | review/review |
| brms / s2_mm | 0.073 [0.071–0.083] | 0.093 [0.071–0.287] | 4.67 | review/review |
| brms / s2_mm_weights | 0.080 [0.067–0.086] | 0.084 [0.071–0.312] | 4.69 | review/review |
| brms / s2_mmc | 0.185 [0.170–0.189] | 0.225 [0.193–0.382] | 6.99 | review/review |
| brms / s2_mo_simo_prior | 0.046 [0.042–0.048] | 0.073 [0.069–0.268] | 3.98 | clear/clear |
| brms / s2_multinomial | 0.164 [0.151–0.171] | 0.218 [0.209–0.417] | 4.01 | clear/clear |
| brms / s2_mv_shared_re | 0.152 [0.142–0.155] | 0.184 [0.168–0.382] | 7.13 | review/review |
| brms / s2_mv_subset | 0.029 [0.027–0.033] | 0.030 [0.028–0.219] | 3.46 | clear/clear |
| brms / s2_nl_noloop | 0.061 [0.053–0.067] | 0.151 [0.126–0.336] | 3.92 | review/review |
| brms / s2_nlf | 0.027 [0.025–0.028] | 0.053 [0.048–0.252] | 3.99 | clear/clear |
| brms / s2_rate | 0.019 [0.018–0.020] | 0.029 [0.027–0.219] | 3.64 | clear/clear |
| brms / s2_s_by | 0.171 [0.168–0.259] | 0.488 [0.482–0.689] | 5.90 | review/review |
| brms / s2_s_cc | 0.040 [0.037–0.041] | 0.083 [0.079–0.294] | 4.30 | review/review |
| brms / s2_sar | 0.066 [0.061–0.067] | 0.137 [0.125–0.343] | 4.06 | clear/clear |
| brms / s2_sar_error | 0.068 [0.064–0.074] | 0.141 [0.126–0.338] | 4.12 | clear/clear |
| brms / s2_shifted_lognormal | 0.027 [0.025–0.029] | 0.044 [0.041–0.239] | 3.93 | clear/clear |
| brms / s2_t2_by | 0.114 [0.111–0.121] | 0.280 [0.270–0.464] | 5.83 | review/review |
| brms / s2_threading | 0.021 [0.019–0.024] | 0.024 [0.022–0.222] | 3.37 | clear/clear |
| brms / s2_unstr | 0.544 [0.535–0.553] | 0.528 [0.464–0.705] | 7.55 | review/review |
| brms / s2_weights_trunc | 0.209 [0.185–0.221] | 0.231 [0.210–0.495] | 3.64 | review/review |
| brms / s2_wiener | 0.458 [0.439–0.469] | 0.473 [0.435–0.644] | 3.67 | clear/clear |
| brms / s2_zi_asymlaplace | 0.209 [0.202–0.226] | 0.145 [0.138–0.343] | 3.60 | clear/clear |
| brms / s2_zi_beta | 0.074 [0.067–0.074] | 0.112 [0.106–0.321] | 3.64 | clear/clear |
| brms / s2_zoi_beta | 0.078 [0.075–0.085] | 0.122 [0.113–0.305] | 3.70 | clear/clear |
| brms / sw_acat | 0.724 [0.697–0.735] | 3.17 [3.11–3.24] | 3.89 | clear/clear |
| brms / sw_acat_cs | 1.29 [1.20–1.35] | 5.33 [5.08–5.56] | 5.03 | clear/clear |
| brms / sw_ar | 0.041 [0.039–0.043] | 0.056 [0.050–0.257] | 4.24 | clear/clear |
| brms / sw_arma | 0.134 [0.122–0.146] | 0.211 [0.175–0.396] | 4.36 | review/review |
| brms / sw_asymlaplace | 0.174 [0.165–0.177] | 0.113 [0.103–0.287] | 3.54 | clear/clear |
| brms / sw_bernoulli | 0.019 [0.018–0.020] | 0.022 [0.020–0.199] | 3.26 | clear/clear |
| brms / sw_beta | 0.037 [0.034–0.043] | 0.053 [0.050–0.243] | 3.87 | clear/clear |
| brms / sw_binomial | 0.032 [0.032–0.034] | 0.040 [0.040–0.247] | 3.57 | clear/clear |
| brms / sw_categorical | 0.039 [0.037–0.041] | 0.037 [0.033–0.254] | 4.32 | clear/clear |
| brms / sw_cens | 0.030 [0.030–0.032] | 0.048 [0.047–0.262] | 4.16 | clear/clear |
| brms / sw_cratio | 0.768 [0.746–0.782] | 1.07 [1.01–1.30] | 3.63 | clear/clear |
| brms / sw_cratio_cs | 1.27 [1.20–1.30] | 1.99 [1.94–2.55] | 4.73 | clear/clear |
| brms / sw_cumulative | 0.311 [0.288–0.324] | 0.317 [0.300–0.489] | 3.78 | clear/clear |
| brms / sw_cumulative_cs | 1.39 [1.22–1.66] | 1.53 [1.25–1.80] | 4.80 | review/review |
| brms / sw_dist_sigma | 0.026 [0.025–0.028] | 0.051 [0.045–0.249] | 3.86 | clear/clear |
| brms / sw_exgaussian | 0.054 [0.052–0.055] | 0.089 [0.076–0.298] | 3.89 | review/review |
| brms / sw_gamma | 0.024 [0.023–0.027] | 0.038 [0.036–0.232] | 3.73 | clear/clear |
| brms / sw_gaussian | 0.019 [0.019–0.024] | 0.023 [0.021–0.164] | 3.29 | clear/clear |
| brms / sw_gp | not measured | not measured | — | incomplete/incomplete |
| brms / sw_hurdle_gamma | 0.043 [0.040–0.045] | 0.070 [0.066–0.209] | 3.55 | clear/clear |
| brms / sw_hurdle_lognormal | 0.036 [0.034–0.038] | 0.062 [0.055–0.227] | 3.59 | clear/clear |
| brms / sw_hurdle_pois | 0.037 [0.035–0.039] | 0.069 [0.064–0.236] | 3.39 | clear/clear |
| brms / sw_lognormal | 0.021 [0.021–0.023] | 0.031 [0.030–0.190] | 3.71 | clear/clear |
| brms / sw_ma | 0.044 [0.040–0.046] | 0.059 [0.059–0.224] | 4.28 | clear/clear |
| brms / sw_me | 0.106 [0.092–0.110] | 0.196 [0.149–0.331] | 4.15 | review/review |
| brms / sw_mi | 0.038 [0.034–0.039] | 0.041 [0.041–0.207] | 3.87 | clear/clear |
| brms / sw_mixture | 1.76 [1.61–1.91] | 2.56 [2.41–2.65] | 4.80 | review/review |
| brms / sw_mono | 0.044 [0.041–0.048] | 0.067 [0.066–0.278] | 4.00 | clear/clear |
| brms / sw_mv_norescor | 0.030 [0.028–0.032] | 0.032 [0.032–0.240] | 3.48 | clear/clear |
| brms / sw_mv_rescor | 0.165 [0.152–0.169] | 0.170 [0.155–0.382] | 6.21 | clear/clear |
| brms / sw_negbinomial | 2.60 [2.07–2.83] | 3.24 [3.10–3.41] | 3.39 | review/review |
| brms / sw_nonlinear | 0.024 [0.022–0.025] | 0.050 [0.045–0.257] | 3.89 | clear/clear |
| brms / sw_poisson | 0.021 [0.020–0.025] | 0.023 [0.022–0.214] | 3.10 | clear/clear |
| brms / sw_re_bern | 0.051 [0.047–0.054] | 0.082 [0.068–0.255] | 4.66 | clear/clear |
| brms / sw_re_gauss | 0.055 [0.049–0.060] | 0.075 [0.073–0.281] | 4.59 | review/review |
| brms / sw_re_negbin | capped 1/4 | 4.97 [0.217–5.83] | 4.74 | incomplete/review |
| brms / sw_re_pois | 0.060 [0.050–0.066] | 0.086 [0.079–0.316] | 4.52 | review/review |
| brms / sw_re_slope | 0.169 [0.160–0.184] | 0.201 [0.180–0.421] | 6.82 | review/review |
| brms / sw_se | 0.017 [0.016–0.018] | 0.024 [0.022–0.202] | 3.50 | clear/clear |
| brms / sw_skewnormal | 0.048 [0.045–0.049] | 0.056 [0.054–0.253] | 3.95 | clear/clear |
| brms / sw_spline_s | 0.062 [0.058–0.072] | 0.161 [0.139–0.368] | 4.46 | review/review |
| brms / sw_spline_t2 | 0.079 [0.075–0.091] | 0.183 [0.159–0.362] | 4.93 | review/review |
| brms / sw_sratio | 0.768 [0.749–0.788] | 1.08 [1.03–1.29] | 3.67 | clear/clear |
| brms / sw_student | 0.027 [0.026–0.029] | 0.040 [0.037–0.257] | 3.85 | clear/clear |
| brms / sw_trunc | 0.206 [0.197–0.215] | 0.238 [0.196–0.454] | 3.56 | review/review |
| brms / sw_vonmises | 0.853 [0.727–1.19] | 1.67 [1.52–1.89] | 3.86 | review/review |
| brms / sw_weibull | 0.039 [0.036–0.043] | 0.057 [0.048–0.258] | 3.89 | clear/clear |
| brms / sw_weights | 0.022 [0.021–0.023] | 0.038 [0.036–0.251] | 3.52 | clear/clear |
| brms / sw_zi_binomial | 0.047 [0.042–0.053] | 0.068 [0.062–0.283] | 3.40 | clear/clear |
| brms / sw_zi_negbin | 0.076 [0.072–0.094] | 0.117 [0.102–0.321] | 3.45 | review/review |
| brms / sw_zi_poisson | 0.034 [0.033–0.038] | 0.051 [0.046–0.262] | 3.39 | clear/clear |

## Incomplete fixtures

- **ch13_m13_6**: stanli seed 3: timeout (limit 2.57 s)
- **i320_gp_expquad**: density/gradient mismatch: scaled error 1.18e-09
- **s2_com_poisson**: s2_com_poisson/gradient/0/stanli: failed (event 2828)
- **s2_gp_by_gr**: density/gradient mismatch: scaled error 7.33e-07
- **s2_invgaussian**: s2_invgaussian/gradient/0/stanli: failed (event 3467)
- **sw_gp**: density/gradient mismatch: scaled error 7.83e-09
- **sw_re_negbin**: stanli seed 2: timeout (limit 0.651 s)

## Reproduce the tables

Retain the original run directory, including model headers, inputs, logs and per-seed CSVs. After the timed sweep finishes, run:

```sh
python3 tools/teaching_diagnostic_jobs.py RUN_DIRECTORY /tmp/teaching-jobs.json
Rscript tools/summarize_rethinking_bench.R /tmp/teaching-jobs.json /tmp/teaching-diagnostics.json
python3 tools/report_teaching.py RUN_DIRECTORY /tmp/teaching-diagnostics.json output/teaching-performance
```

The jobs file contains absolute CSV paths; regenerate it after relocating the evidence directory. The exporter refuses an unfinished sweep. See the [benchmark protocol](../../docs/benchmark-protocol.md).
