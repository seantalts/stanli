# Teaching model performance

Fresh Release build on Apple M3 Ultra, 96 GiB RAM, macOS ARM64. Runtime and compiler sources match main `2ae6c1d0`; the manifest records the exact branch, dependencies, and executable hashes.

Run `fae5494c296cd547` includes every one of its 199 fixtures. Failures and timeouts remain in the tables.

## Collection summary

| Collection | Fixtures | Completed in both | Lower Stanli CLI time | Median CmdStan/Stanli CLI ratio | Screen clear in both |
| --- | ---: | ---: | ---: | ---: | ---: |
| educational | 13 | 13 | 12/13 | 1.31 | 10/13 |
| rethinking | 62 | 59 | 55/59 | 1.48 | 48/59 |
| brms | 124 | 111 | 88/111 | 1.35 | 69/111 |

A ratio above one means less elapsed time for Stanli. Each model has equal weight in the median; these ratios describe the completed fixtures only. Diagnostic flags are not removed from the timing summary.

## What was measured

Each engine ran 4 independent single-chain seeds, with 1000 warmup iterations and 1000 retained draws per seed, target acceptance 0.8, tree depth 10, and random initialization. The table reports median [minimum–maximum] CLI seconds. Stanli includes model preparation; CmdStan starts from a compiled model. Both include generated quantities and CSV output. Toolchain installation is excluded.

CSV precision follows the CLI defaults: eight significant digits for CmdStan 2.39 and 17 for Stanli. The numerical oracle uses separate high-precision values; sampling diagnostics use these retained CSVs.

CmdStan compilation is measured once, including Stan translation and the C++ model build. Adding it to the median CmdStan CLI time gives an estimated first fit; this is a sum of measured stages. It is not a directly timed four-chain R fit or a cold-cache measurement.

CmdStan ran first for each seed. Stanli's cap was min(3 × that CmdStan CLI time, 900 seconds). A failed, invalid, or capped seed prevents an aggregate for that engine. A preceding preparation or gradient-gate failure leaves sampling unmeasured. No partial-seed averages are substituted.

Screen clear: no retained-draw divergences or depth hits, finite R-hat ≤ 1.01 and bulk ESS ≥ 400 for every nonconstant variable in the parameters block. Structural fixed matrix entries are checked and omitted. The CSV also records tail ESS, minimum E-BFMI, and minimum bulk ESS divided by the sum of the four serial CLI durations. These are descriptive diagnostics; fixed-budget runtime is not time to equal inferential accuracy.

Gradient results in the CSV use six alternating pairs at the same parameter point. Each pair must satisfy the full density/gradient scaled-error gate of 1e-9. The separate three-point numerical replay and its known exceptions are described in the [support guide](../../docs/teaching-support.md).

## Full appendix

| Collection / fixture | Stanli CLI | CmdStan CLI | CmdStan compile | Screen S/C |
| --- | ---: | ---: | ---: | --- |
| educational / aalto_bern | 0.012 [0.011–0.017] | 0.015 [0.014–0.202] | 2.57 | clear/clear |
| educational / aalto_binom | 0.011 [0.011–0.011] | 0.012 [0.012–0.197] | 2.38 | clear/clear |
| educational / aalto_binom2 | 0.013 [0.013–0.015] | 0.015 [0.014–0.216] | 2.52 | clear/clear |
| educational / aalto_binomb | 0.012 [0.010–0.016] | 0.014 [0.011–0.215] | 2.41 | clear/clear |
| educational / aalto_gpareto | 0.028 [0.027–0.031] | 0.025 [0.024–0.227] | 2.93 | clear/clear |
| educational / aalto_grp_aov | 0.017 [0.016–0.018] | 0.024 [0.022–0.201] | 3.09 | clear/clear |
| educational / aalto_grp_prior_mean | 0.028 [0.024–0.033] | 0.033 [0.032–0.238] | 3.16 | review/review |
| educational / aalto_grp_prior_mean_var | 0.061 [0.057–0.068] | 0.102 [0.091–0.310] | 3.92 | review/review |
| educational / aalto_lin | 0.021 [0.021–0.024] | 0.030 [0.027–0.224] | 3.07 | clear/review |
| educational / aalto_lin_std | 0.019 [0.018–0.020] | 0.026 [0.023–0.213] | 3.27 | clear/clear |
| educational / aalto_lin_std_t | 0.021 [0.020–0.026] | 0.028 [0.027–0.211] | 3.50 | clear/clear |
| educational / aalto_poisson_hurdle | 0.647 [0.643–0.660] | 0.822 [0.789–1.07] | 2.94 | clear/clear |
| educational / aalto_poisson_simple | 0.100 [0.093–0.103] | 0.157 [0.153–0.388] | 2.66 | clear/clear |
| rethinking / ch09_m5_8s | 1.24 [1.22–1.26] | 3.57 [3.48–3.73] | 2.92 | review/review |
| rethinking / ch09_m5_8s2 | 0.873 [0.318–1.17] | 2.59 [1.12–3.33] | 2.86 | review/review |
| rethinking / ch09_m9_1 | 0.030 [0.025–0.033] | 0.053 [0.046–0.257] | 3.09 | clear/clear |
| rethinking / ch09_m9_1_chains4 | 0.030 [0.026–0.032] | 0.056 [0.046–0.254] | 3.06 | clear/clear |
| rethinking / ch09_m9_2 | 0.018 [0.017–0.019] | 0.025 [0.023–0.245] | 2.50 | review/review |
| rethinking / ch09_m9_3 | 0.014 [0.013–0.014] | 0.017 [0.017–0.233] | 2.52 | clear/clear |
| rethinking / ch09_m9_4 | 0.970 [0.943–0.978] | 1.41 [1.36–1.58] | 2.55 | review/review |
| rethinking / ch09_m9_5 | 0.270 [0.264–0.280] | 0.354 [0.323–0.563] | 2.56 | clear/clear |
| rethinking / ch09_mp | 0.022 [0.014–0.028] | 0.067 [0.020–0.245] | 2.29 | clear/review |
| rethinking / ch11_m11_10 | 0.020 [0.020–0.021] | 0.027 [0.026–0.221] | 3.25 | clear/clear |
| rethinking / ch11_m11_11 | 0.141 [0.132–0.154] | 0.219 [0.197–0.420] | 3.54 | review/clear |
| rethinking / ch11_m11_4 | 0.207 [0.206–0.218] | 0.395 [0.387–0.612] | 3.02 | clear/clear |
| rethinking / ch11_m11_5 | 0.320 [0.304–0.322] | 0.653 [0.636–0.881] | 3.09 | clear/clear |
| rethinking / ch11_m11_6 | 0.038 [0.035–0.039] | 0.052 [0.049–0.254] | 3.02 | clear/clear |
| rethinking / ch11_m11_7 | 0.014 [0.013–0.015] | 0.018 [0.017–0.198] | 2.81 | clear/clear |
| rethinking / ch11_m11_8 | 0.051 [0.048–0.055] | 0.078 [0.071–0.272] | 2.93 | review/review |
| rethinking / ch11_m11_9 | 0.012 [0.011–0.014] | 0.015 [0.013–0.192] | 2.60 | clear/clear |
| rethinking / ch11_m_pois | 0.012 [0.012–0.013] | 0.014 [0.014–0.212] | 2.51 | clear/clear |
| rethinking / ch12_m12_1 | 0.030 [0.029–0.031] | 0.041 [0.037–0.246] | 3.21 | clear/clear |
| rethinking / ch12_m12_2 | 0.107 [0.095–0.124] | 0.159 [0.134–0.356] | 3.97 | clear/clear |
| rethinking / ch12_m12_3 | 0.028 [0.027–0.030] | 0.257 [0.236–0.446] | 2.50 | clear/clear |
| rethinking / ch12_m12_3_alt | 0.054 [0.053–0.061] | 0.257 [0.227–0.426] | 2.48 | clear/clear |
| rethinking / ch12_m12_4 | 0.672 [0.640–0.689] | 86.08 [81.09–89.26] | 3.19 | clear/clear |
| rethinking / ch12_m12_5 | 283.2 [272.6–290.5] | 161.5 [158.9–165.8] | 3.57 | clear/clear |
| rethinking / ch12_m12_6 | capped 1/4 | 485.0 [454.9–508.2] | 4.24 | incomplete/clear |
| rethinking / ch12_m12_7 | 381.5 [309.9–389.2] | 214.0 [191.2–227.7] | 3.55 | clear/clear |
| rethinking / ch13_m13_1 | 0.052 [0.048–0.054] | 0.073 [0.069–0.291] | 2.96 | clear/clear |
| rethinking / ch13_m13_2 | 0.058 [0.056–0.061] | 0.086 [0.080–0.298] | 3.11 | clear/clear |
| rethinking / ch13_m13_3 | 0.069 [0.063–0.079] | 0.080 [0.076–0.279] | 3.04 | clear/clear |
| rethinking / ch13_m13_4 | 0.490 [0.415–0.622] | 0.777 [0.675–1.13] | 3.42 | review/review |
| rethinking / ch13_m13_4b | 0.406 [0.339–0.546] | 0.607 [0.513–0.958] | 3.31 | review/review |
| rethinking / ch13_m13_4nc | 0.744 [0.724–0.790] | 1.90 [1.84–2.11] | 3.33 | clear/clear |
| rethinking / ch13_m13_5 | 0.243 [0.237–0.248] | 0.438 [0.414–0.631] | 3.24 | clear/clear |
| rethinking / ch13_m13_6 | capped 1/4 | 1.02 [0.872–1.16] | 3.40 | incomplete/review |
| rethinking / ch13_m13_7 | 0.016 [0.014–0.018] | 0.022 [0.020–0.219] | 2.30 | review/review |
| rethinking / ch13_m13_7nc | 0.012 [0.011–0.013] | 0.014 [0.014–0.211] | 2.34 | clear/clear |
| rethinking / ch14_m14_1 | 0.314 [0.305–0.318] | 0.403 [0.391–0.665] | 6.92 | clear/clear |
| rethinking / ch14_m14_10 | 6.14 [5.66–6.29] | 6.17 [6.07–6.30] | 4.06 | clear/clear |
| rethinking / ch14_m14_11 | not measured | not measured | — | incomplete/incomplete |
| rethinking / ch14_m14_2 | 2.61 [2.22–3.12] | 3.74 [3.46–4.49] | 7.08 | review/review |
| rethinking / ch14_m14_3 | 0.993 [0.970–1.03] | 1.74 [1.72–1.97] | 5.92 | clear/clear |
| rethinking / ch14_m14_4 | 0.031 [0.029–0.035] | 0.079 [0.072–0.278] | 2.81 | clear/clear |
| rethinking / ch14_m14_4x | 0.032 [0.030–0.034] | 0.077 [0.075–0.264] | 2.79 | clear/clear |
| rethinking / ch14_m14_5 | 0.052 [0.046–0.053] | 0.166 [0.150–0.351] | 2.82 | clear/clear |
| rethinking / ch14_m14_6 | 2.73 [2.67–2.90] | 4.04 [3.81–4.11] | 6.67 | clear/clear |
| rethinking / ch14_m14_6x | 2.77 [2.64–2.88] | 4.01 [3.85–4.10] | 6.67 | clear/clear |
| rethinking / ch14_m14_7 | 4.60 [4.51–4.69] | 6.55 [6.47–6.62] | 8.86 | clear/clear |
| rethinking / ch14_m14_8 | 0.839 [0.815–0.901] | 1.05 [0.947–1.06] | 4.48 | clear/clear |
| rethinking / ch14_m14_8nc | 0.807 [0.732–1.00] | 1.19 [1.11–1.58] | 5.72 | clear/clear |
| rethinking / ch14_m14_9 | 6.21 [6.09–6.82] | 6.14 [5.80–6.56] | 4.06 | clear/clear |
| rethinking / ch15_m15_1 | 0.049 [0.048–0.054] | 0.103 [0.093–0.315] | 3.20 | clear/clear |
| rethinking / ch15_m15_2 | 0.083 [0.074–0.084] | 0.140 [0.131–0.359] | 3.43 | clear/clear |
| rethinking / ch15_m15_3 | 0.240 [0.231–0.245] | 0.305 [0.296–0.523] | 2.68 | clear/clear |
| rethinking / ch15_m15_4 | 0.206 [0.193–0.222] | 0.259 [0.238–0.456] | 2.64 | clear/clear |
| rethinking / ch15_m15_5 | 0.037 [0.033–0.041] | 0.064 [0.060–0.266] | 3.23 | clear/clear |
| rethinking / ch15_m15_6 | 0.022 [0.022–0.024] | 0.033 [0.030–0.213] | 2.97 | clear/clear |
| rethinking / ch15_m15_7 | 0.586 [0.547–0.648] | 0.553 [0.533–0.759] | 7.18 | clear/clear |
| rethinking / ch15_m15_8 | 0.052 [0.051–0.056] | 0.113 [0.103–0.310] | 2.83 | clear/clear |
| rethinking / ch15_m15_9 | 0.072 [0.072–0.078] | 0.158 [0.149–0.342] | 3.00 | clear/clear |
| rethinking / ch16_m16_1 | 3.60 [3.56–3.66] | 8.58 [8.49–8.70] | 3.02 | clear/clear |
| rethinking / ch16_m16_4 | 0.081 [0.070–0.088] | 0.235 [0.219–0.413] | 2.90 | clear/clear |
| rethinking / extra_hurdle_poisson | 0.014 [0.014–0.017] | 0.018 [0.018–0.221] | 2.59 | clear/clear |
| brms / i319_gauss_re | 0.233 [0.212–0.252] | 0.282 [0.254–0.456] | 4.61 | clear/clear |
| brms / i319_negbin_fixed | 0.196 [0.189–0.203] | 0.183 [0.148–0.409] | 3.39 | clear/review |
| brms / i319_negbin_re | 0.869 [0.793–0.886] | 1.07 [1.03–1.23] | 4.79 | clear/clear |
| brms / i319_pois_fixed | 0.093 [0.087–0.101] | 0.102 [0.094–0.291] | 3.11 | clear/clear |
| brms / i319_pois_re | 0.977 [0.949–1.08] | 1.28 [1.26–1.46] | 4.51 | clear/review |
| brms / i319_pois_re2 | 1.75 [1.70–1.78] | 2.52 [2.36–2.61] | 4.82 | clear/clear |
| brms / i320_gp_expquad | not measured | not measured | — | incomplete/incomplete |
| brms / i320_gp_matern32 | 2.37 [1.91–2.62] | 1.83 [1.31–2.02] | 6.98 | review/review |
| brms / i320_mi_nhanes | 0.699 [0.690–0.752] | 0.910 [0.870–1.05] | 4.42 | clear/review |
| brms / i320_pois_trunc_both | 1.10 [1.08–1.12] | 0.799 [0.763–0.965] | 3.55 | clear/clear |
| brms / i320_pois_trunc_ub | 0.963 [0.931–0.978] | 0.649 [0.631–0.847] | 3.49 | clear/clear |
| brms / i320_sratio_cs | capped 3/4 | 2.72 [2.56–2.98] | 5.03 | incomplete/clear |
| brms / i320_sratio_plain | capped 4/4 | 1.51 [1.47–1.72] | 3.78 | incomplete/clear |
| brms / s2_ar_cov | 0.112 [0.109–0.119] | 0.117 [0.104–0.324] | 7.24 | clear/clear |
| brms / s2_beta_binomial | 0.067 [0.063–0.073] | 0.082 [0.075–0.281] | 3.72 | clear/clear |
| brms / s2_car | 0.069 [0.058–0.074] | 0.120 [0.114–0.341] | 4.38 | review/review |
| brms / s2_car_esicar | 0.058 [0.053–0.064] | 0.087 [0.079–0.303] | 4.51 | review/review |
| brms / s2_car_icar | 1.99 [1.96–2.04] | 4.25 [4.14–4.51] | 4.41 | review/review |
| brms / s2_categorical_re | 0.448 [0.437–0.456] | 0.363 [0.334–0.545] | 5.43 | clear/review |
| brms / s2_cens_interval | 0.065 [0.060–0.071] | 0.047 [0.044–0.234] | 3.77 | clear/clear |
| brms / s2_com_poisson | not measured | not measured | — | incomplete/incomplete |
| brms / s2_cosy | 0.140 [0.131–0.162] | 0.152 [0.143–0.331] | 6.49 | clear/clear |
| brms / s2_cox | 0.058 [0.053–0.060] | 0.115 [0.101–0.321] | 4.16 | clear/clear |
| brms / s2_cox_cens | 0.053 [0.047–0.057] | 0.116 [0.103–0.338] | 5.09 | clear/clear |
| brms / s2_cumulative_cauchit | 0.044 [0.043–0.048] | 0.123 [0.105–0.298] | 3.66 | clear/clear |
| brms / s2_cumulative_cloglog | 0.048 [0.043–0.052] | 0.099 [0.086–0.293] | 3.67 | clear/clear |
| brms / s2_cumulative_probit | 0.288 [0.277–0.323] | 0.116 [0.104–0.328] | 3.72 | clear/clear |
| brms / s2_custom_vint | 0.068 [0.066–0.070] | 0.088 [0.081–0.275] | 3.53 | clear/clear |
| brms / s2_custom_vreal | 0.020 [0.019–0.023] | 0.041 [0.035–0.253] | 3.48 | clear/clear |
| brms / s2_dirichlet | 0.374 [0.363–0.385] | 0.282 [0.281–0.471] | 4.39 | clear/clear |
| brms / s2_discrete_weibull | 0.086 [0.083–0.090] | 0.171 [0.143–0.377] | 3.48 | clear/clear |
| brms / s2_dist_sigma_re | 0.068 [0.063–0.077] | 0.121 [0.109–0.339] | 4.96 | review/review |
| brms / s2_fcor | 0.183 [0.163–0.193] | 0.178 [0.178–0.367] | 4.91 | clear/clear |
| brms / s2_frechet | 0.090 [0.081–0.099] | 0.143 [0.133–0.326] | 3.94 | clear/clear |
| brms / s2_gev | 0.248 [0.240–0.254] | 0.092 [0.090–0.276] | 3.79 | clear/clear |
| brms / s2_gp_approx | 0.054 [0.047–0.063] | 0.143 [0.105–0.314] | 5.23 | review/review |
| brms / s2_gp_by_approx | 0.084 [0.077–0.092] | 0.224 [0.211–0.402] | 6.02 | review/review |
| brms / s2_gp_by_gr | not measured | not measured | — | incomplete/incomplete |
| brms / s2_gr_by | 0.075 [0.060–0.090] | 0.101 [0.087–0.280] | 4.92 | review/review |
| brms / s2_gr_student | 0.106 [0.102–0.125] | 0.169 [0.149–0.388] | 5.19 | review/review |
| brms / s2_hurdle_cumulative | 0.272 [0.237–0.282] | 0.174 [0.169–0.406] | 4.19 | clear/clear |
| brms / s2_hurdle_negbin | 0.089 [0.080–0.098] | 0.152 [0.150–0.380] | 3.48 | review/review |
| brms / s2_index_mi | 0.051 [0.045–0.056] | 0.065 [0.057–0.274] | 4.22 | clear/clear |
| brms / s2_invgaussian | not measured | not measured | — | incomplete/incomplete |
| brms / s2_logistic_normal | 0.407 [0.403–0.413] | 0.470 [0.456–0.653] | 6.34 | clear/clear |
| brms / s2_me2 | 0.314 [0.285–0.335] | 0.441 [0.392–0.684] | 6.75 | clear/review |
| brms / s2_me2_nomecor | 0.182 [0.171–0.198] | 0.322 [0.301–0.503] | 4.45 | review/review |
| brms / s2_mi_lognormal | 0.037 [0.036–0.038] | 0.058 [0.054–0.261] | 4.63 | clear/clear |
| brms / s2_mi_trunc_lb | 0.196 [0.173–0.247] | 0.265 [0.218–0.402] | 4.46 | review/review |
| brms / s2_mixture_theta | 2.09 [1.54–2.51] | 1.64 [0.770–3.68] | 4.58 | review/review |
| brms / s2_mm | 0.077 [0.070–0.083] | 0.090 [0.079–0.319] | 4.72 | review/review |
| brms / s2_mm_weights | 0.082 [0.067–0.089] | 0.085 [0.071–0.313] | 4.70 | review/review |
| brms / s2_mmc | 0.181 [0.169–0.203] | 0.222 [0.192–0.371] | 7.00 | review/review |
| brms / s2_mo_simo_prior | 0.053 [0.051–0.056] | 0.075 [0.070–0.284] | 3.99 | clear/clear |
| brms / s2_multinomial | 0.323 [0.320–0.328] | 0.203 [0.199–0.413] | 4.01 | clear/clear |
| brms / s2_mv_shared_re | 0.147 [0.141–0.156] | 0.183 [0.175–0.365] | 7.14 | review/review |
| brms / s2_mv_subset | 0.028 [0.027–0.033] | 0.030 [0.027–0.206] | 3.48 | clear/clear |
| brms / s2_nl_noloop | 0.064 [0.057–0.067] | 0.157 [0.120–0.374] | 3.94 | review/review |
| brms / s2_nlf | 0.026 [0.025–0.028] | 0.051 [0.046–0.236] | 3.98 | clear/clear |
| brms / s2_rate | 0.018 [0.017–0.019] | 0.027 [0.025–0.208] | 3.63 | clear/clear |
| brms / s2_s_by | 0.170 [0.166–0.250] | 0.489 [0.483–0.711] | 5.92 | review/review |
| brms / s2_s_cc | 0.039 [0.037–0.043] | 0.083 [0.077–0.292] | 4.32 | review/review |
| brms / s2_sar | 0.063 [0.061–0.066] | 0.140 [0.125–0.365] | 4.07 | clear/clear |
| brms / s2_sar_error | 0.065 [0.059–0.073] | 0.142 [0.128–0.342] | 4.11 | clear/clear |
| brms / s2_shifted_lognormal | 0.028 [0.026–0.029] | 0.046 [0.041–0.243] | 3.93 | clear/clear |
| brms / s2_t2_by | 0.117 [0.113–0.118] | 0.276 [0.273–0.448] | 5.85 | review/review |
| brms / s2_threading | 0.021 [0.020–0.022] | 0.023 [0.022–0.209] | 3.37 | clear/clear |
| brms / s2_unstr | 0.543 [0.526–0.554] | 0.538 [0.448–0.715] | 7.51 | review/review |
| brms / s2_weights_trunc | 0.498 [0.402–0.559] | 0.230 [0.193–0.456] | 3.63 | review/review |
| brms / s2_wiener | 0.727 [0.680–0.741] | 0.476 [0.424–0.659] | 3.67 | clear/clear |
| brms / s2_zi_asymlaplace | capped 3/4 | 0.154 [0.139–0.370] | 3.60 | incomplete/clear |
| brms / s2_zi_beta | 0.215 [0.190–0.218] | 0.115 [0.106–0.323] | 3.65 | clear/clear |
| brms / s2_zoi_beta | 0.243 [0.236–0.252] | 0.122 [0.115–0.309] | 3.68 | clear/clear |
| brms / sw_acat | 0.732 [0.694–0.754] | 3.15 [3.10–3.24] | 3.89 | clear/clear |
| brms / sw_acat_cs | 1.30 [1.21–1.33] | 5.35 [5.14–5.60] | 5.02 | clear/clear |
| brms / sw_ar | 0.043 [0.039–0.045] | 0.058 [0.056–0.276] | 4.27 | clear/clear |
| brms / sw_arma | 0.136 [0.119–0.154] | 0.207 [0.180–0.433] | 4.38 | review/review |
| brms / sw_asymlaplace | capped 2/4 | 0.118 [0.115–0.281] | 3.54 | incomplete/clear |
| brms / sw_bernoulli | 0.020 [0.019–0.024] | 0.023 [0.021–0.230] | 3.28 | clear/clear |
| brms / sw_beta | 0.038 [0.033–0.040] | 0.052 [0.050–0.233] | 3.89 | clear/clear |
| brms / sw_binomial | 0.029 [0.029–0.033] | 0.038 [0.036–0.226] | 3.54 | clear/clear |
| brms / sw_categorical | 0.037 [0.034–0.039] | 0.036 [0.034–0.234] | 4.31 | clear/clear |
| brms / sw_cens | 0.030 [0.026–0.032] | 0.046 [0.043–0.268] | 4.17 | clear/clear |
| brms / sw_cratio | capped 4/4 | 1.07 [1.02–1.30] | 3.62 | incomplete/clear |
| brms / sw_cratio_cs | capped 4/4 | 1.99 [1.95–2.17] | 4.73 | incomplete/clear |
| brms / sw_cumulative | 0.350 [0.331–0.364] | 0.322 [0.319–0.481] | 3.82 | clear/clear |
| brms / sw_cumulative_cs | 1.40 [1.25–1.62] | 1.54 [1.28–1.82] | 4.77 | review/review |
| brms / sw_dist_sigma | 0.027 [0.026–0.027] | 0.049 [0.048–0.260] | 3.83 | clear/clear |
| brms / sw_exgaussian | 0.055 [0.047–0.061] | 0.084 [0.077–0.281] | 3.90 | review/review |
| brms / sw_gamma | 0.022 [0.021–0.025] | 0.038 [0.033–0.215] | 3.74 | clear/clear |
| brms / sw_gaussian | 0.022 [0.021–0.026] | 0.024 [0.023–0.232] | 3.31 | clear/clear |
| brms / sw_gp | not measured | not measured | — | incomplete/incomplete |
| brms / sw_hurdle_gamma | 0.121 [0.116–0.126] | 0.075 [0.065–0.301] | 3.54 | clear/clear |
| brms / sw_hurdle_lognormal | 0.110 [0.100–0.111] | 0.061 [0.056–0.239] | 3.57 | clear/clear |
| brms / sw_hurdle_pois | 0.038 [0.035–0.040] | 0.069 [0.062–0.251] | 3.38 | clear/clear |
| brms / sw_lognormal | 0.019 [0.019–0.021] | 0.030 [0.027–0.211] | 3.68 | clear/clear |
| brms / sw_ma | 0.042 [0.041–0.045] | 0.057 [0.053–0.262] | 4.25 | clear/clear |
| brms / sw_me | 0.102 [0.096–0.107] | 0.189 [0.160–0.369] | 4.15 | review/review |
| brms / sw_mi | 0.036 [0.034–0.038] | 0.039 [0.036–0.243] | 3.85 | clear/clear |
| brms / sw_mixture | 1.80 [1.69–2.05] | 2.58 [2.42–2.70] | 4.82 | review/review |
| brms / sw_mono | 0.051 [0.046–0.056] | 0.066 [0.064–0.275] | 3.98 | clear/clear |
| brms / sw_mv_norescor | 0.029 [0.027–0.031] | 0.031 [0.028–0.238] | 3.46 | clear/clear |
| brms / sw_mv_rescor | 0.161 [0.158–0.165] | 0.169 [0.161–0.388] | 6.22 | clear/clear |
| brms / sw_negbinomial | 2.66 [2.10–2.85] | 3.25 [3.13–3.45] | 3.39 | review/review |
| brms / sw_nonlinear | 0.024 [0.023–0.025] | 0.053 [0.048–0.244] | 3.91 | clear/clear |
| brms / sw_poisson | 0.022 [0.022–0.025] | 0.025 [0.024–0.241] | 3.12 | clear/clear |
| brms / sw_re_bern | 0.050 [0.047–0.054] | 0.084 [0.064–0.267] | 4.66 | clear/clear |
| brms / sw_re_gauss | 0.052 [0.050–0.061] | 0.071 [0.067–0.258] | 4.57 | review/review |
| brms / sw_re_negbin | capped 1/4 | 5.00 [0.201–5.87] | 4.75 | incomplete/review |
| brms / sw_re_pois | 0.060 [0.050–0.067] | 0.085 [0.079–0.292] | 4.53 | review/review |
| brms / sw_re_slope | 0.169 [0.159–0.186] | 0.199 [0.185–0.420] | 6.84 | review/review |
| brms / sw_se | 0.016 [0.015–0.022] | 0.024 [0.021–0.231] | 3.52 | clear/clear |
| brms / sw_skewnormal | 0.048 [0.045–0.053] | 0.062 [0.057–0.276] | 3.98 | clear/clear |
| brms / sw_spline_s | 0.065 [0.062–0.067] | 0.162 [0.141–0.325] | 4.47 | review/review |
| brms / sw_spline_t2 | 0.080 [0.075–0.087] | 0.173 [0.165–0.397] | 4.94 | review/review |
| brms / sw_sratio | capped 3/4 | 1.09 [1.04–1.33] | 3.64 | incomplete/clear |
| brms / sw_student | 0.026 [0.025–0.027] | 0.039 [0.039–0.232] | 3.87 | clear/clear |
| brms / sw_trunc | 0.505 [0.456–0.526] | 0.242 [0.191–0.448] | 3.58 | review/review |
| brms / sw_vonmises | 0.876 [0.733–1.25] | 1.69 [1.53–1.88] | 3.70 | review/review |
| brms / sw_weibull | 0.039 [0.036–0.043] | 0.051 [0.050–0.232] | 3.88 | clear/clear |
| brms / sw_weights | 0.023 [0.022–0.024] | 0.039 [0.038–0.255] | 3.51 | clear/clear |
| brms / sw_zi_binomial | 0.045 [0.042–0.053] | 0.068 [0.062–0.280] | 3.40 | clear/clear |
| brms / sw_zi_negbin | 0.076 [0.072–0.095] | 0.121 [0.102–0.321] | 3.46 | review/review |
| brms / sw_zi_poisson | 0.034 [0.033–0.037] | 0.051 [0.046–0.268] | 3.40 | clear/clear |

## Incomplete fixtures

- **ch12_m12_6**: stanli seed 1: timeout (limit 900 s)
- **ch13_m13_6**: stanli seed 3: timeout (limit 2.62 s)
- **ch14_m14_11**: ch14_m14_11/gradient/0/stanli: timeout (event 1533)
- **i320_gp_expquad**: density/gradient mismatch: scaled error 1.18e-09
- **i320_sratio_cs**: stanli seed 2: timeout (limit 8.2 s); stanli seed 3: timeout (limit 7.67 s); stanli seed 4: timeout (limit 8.12 s)
- **i320_sratio_plain**: stanli seed 1: timeout (limit 5.16 s); stanli seed 2: timeout (limit 4.6 s); stanli seed 3: timeout (limit 4.41 s); stanli seed 4: timeout (limit 4.46 s)
- **s2_com_poisson**: s2_com_poisson/gradient/0/stanli: failed (event 2802)
- **s2_gp_by_gr**: density/gradient mismatch: scaled error 7.33e-07
- **s2_invgaussian**: s2_invgaussian/gradient/0/stanli: failed (event 3441)
- **s2_zi_asymlaplace**: stanli seed 2: timeout (limit 0.465 s); stanli seed 3: timeout (limit 0.46 s); stanli seed 4: timeout (limit 0.418 s)
- **sw_asymlaplace**: stanli seed 3: timeout (limit 0.361 s); stanli seed 4: timeout (limit 0.349 s)
- **sw_cratio**: stanli seed 1: timeout (limit 3.9 s); stanli seed 2: timeout (limit 3.15 s); stanli seed 3: timeout (limit 3.05 s); stanli seed 4: timeout (limit 3.27 s)
- **sw_cratio_cs**: stanli seed 1: timeout (limit 6.5 s); stanli seed 2: timeout (limit 6.02 s); stanli seed 3: timeout (limit 5.85 s); stanli seed 4: timeout (limit 5.92 s)
- **sw_gp**: density/gradient mismatch: scaled error 7.83e-09
- **sw_re_negbin**: stanli seed 2: timeout (limit 0.603 s)
- **sw_sratio**: stanli seed 2: timeout (limit 3.24 s); stanli seed 3: timeout (limit 3.11 s); stanli seed 4: timeout (limit 3.28 s)

The three GP failures (`i320_gp_expquad`, `s2_gp_by_gr`, `sw_gp`) exceeded this benchmark's strict 1e-9 gradient gate. They are the already documented ill-conditioned fixtures; the numerical replay applies its existing recorded exceptions. `s2_com_poisson` is the known support gap, and `s2_invgaussian` has an invalid shared reference point. See the [brms corpus notes](../../tests/brms/README.md). These models were not sampled in this run.

Performance follow-ups: [Rethinking #373](https://github.com/seantalts/stanli/issues/373) and [brms #374](https://github.com/seantalts/stanli/issues/374). The current-build Rethinking preparation failure is [#372](https://github.com/seantalts/stanli/issues/372).

## Reproduce the tables

Retain the original run directory, including model headers, inputs, logs and per-seed CSVs. After the timed sweep finishes, run:

```sh
python3 tools/teaching_diagnostic_jobs.py RUN_DIRECTORY /tmp/teaching-jobs.json
Rscript tools/summarize_rethinking_bench.R /tmp/teaching-jobs.json /tmp/teaching-diagnostics.json
python3 tools/report_teaching.py RUN_DIRECTORY /tmp/teaching-diagnostics.json output/teaching-performance
```

The jobs file contains absolute CSV paths; regenerate it after relocating the evidence directory. The exporter refuses an unfinished sweep. See the [benchmark protocol](../../docs/benchmark-protocol.md).
