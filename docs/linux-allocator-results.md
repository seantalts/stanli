# Linux private allocator: matched rollout results

## Decision

Neither Linux architecture **passes the frozen default-promotion gate**.
Large-normal and hierarchical-GP wins do not cancel x86_64's confirmed
source-to-Fit regressions or ARM64's confirmed HMM/diamonds warm-gradient
regressions. Smaller consistent negatives and unstable cells remain visible.
PR #363 remains draft with auto-merge disabled; no production default has
been merged. Both architectures have completed the frozen measurement plan;
the result is a blocked promotion, not merely unfinished testing.

These are the requested shipping shared-library/Python measurements, not the
older CLI/CmdStan benchmark boundary. No Stan Math/Eigen edits, loop-layout
change, custom allocator purge setting, allocation-size threshold, or shipping
warmup is included.

## Provenance and interpretation

- [Frozen protocol](linux-allocator-rollout.md), committed before measurements.
- [Machine-readable identities and artifact audit](linux-allocator-identities.json).
- Measured source: [3637822b](https://github.com/seantalts/stanli/tree/3637822bedb77d2b98fa1fb7d2f4a2f8d933be3c), baseline main `10f94763`.
- Rebased integration source tested: `395ad117` on main `f7723961`. Its only tree
  difference from the measured source is main's 17-line sanitizer-tool
  installation fix in CMakeLists.txt. Runtime, compiler, allocator, evaluator
  and test sources are byte-identical. The installation fix is conditional
  on the shared sanitizer core, not the measured Release build.
- Independently downloaded pre/post-rebase Linux Clang shipping libraries
  are byte-identical, SHA-256
  `04f864bad8ff093ad620439264e933c9f02ccc15cdbd817e17a285a60bfff0ce`.
- [Raw run](https://github.com/seantalts/stanli/actions/runs/34758730953):
  [x86_64 full artifact](https://github.com/seantalts/stanli/actions/runs/34758730953/artifacts/10319589156).
  Retention is 90 days. It includes binaries, compiler/configuration identities,
  commands, host/cgroup metadata, every stdout/stderr, graph, full gradient and
  Fit snapshots, frozen inputs/calibration, all timing records and summaries.
  The original dirty research worktree and earlier research archive are preserved.
- x86_64 host: Intel Xeon 6973P-C, four logical CPUs, GCC 14.2.1,
  manylinux glibc 2.28, Python 3.12/NumPy 2.5.3. No BLAS thread-variable
  overrides or CPU quota; 8-worker cases oversubscribe this four-CPU host.
- Ratios are **SYSTEM time / private time**, so >1 means faster.
  Tables show the median paired round ratio and full min/max range, plus
  the number of positive rounds. Absolute times are medians across processes;
  dividing those medians need not equal the paired median ratio.
  A/A columns compare aliases of the exact same binary. They expose noise;
  they are not alternative candidates or independent hardware repetitions.
- Warm gradients: five rounds, two processes/slot/round, three timed blocks
  per process, at least 500 ms of actual gradient work per worker before timing.
  Fixed seven-round confirmation is reported separately, never pooled with
  the screen. Source-to-Fit: five rounds, one process/slot/round,
  500 adaptation + 500 sampling transitions with full returned outputs;
  no artificial warmup. Source-to-Fit excludes import; process/import costs
  remain in raw records, and import is shown separately below.
- The 0.97 screening ratio is a diagnostic trigger, **not** a regression budget.
  A negative result below that threshold requires one fixed confirmation.
  Smaller consistent negatives still require review. No repeated resampling
  or changed threshold was used.

## x86_64 correctness, ownership and memory

The downloaded artifacts were independently rehashed against their manifests:
all four libraries, all model inputs, 2,464 gradient snapshots and graph
dimensions, 476 Fit snapshots, and 72 retention snapshots pass. Native warmup
logs confirm every worker reached at least 500 ms. Both allocator variants
passed their full CTest and Python suites before measurement. Every collection
command returned zero; collection success is not promotion approval.

Each retention process ran 1,179,648 timed gradients plus checks/warmup across
12 small/large/small lifetimes. There is no observed unbounded growth in this
bounded run, but private allocation retains materially more memory.

| Retention metric | SYSTEM MiB | Private MiB |
|---|---:|---:|
| Initial RSS | 35.71 | 38.03 |
| Peak RSS | 87.29 | 100.23 |
| Final destroyed-model RSS | 42.33 | 74.97 |
| Destroyed-model range, last six cycles | 42.33–46.35 | 74.97–80.54 |

Final RSS is about 32.6 MiB higher. A plateau is not proof of no leaks for
all lifetimes or of an acceptable memory tradeoff. No purge/collection knob
was tuned after observing this result.

## x86_64 interpretation

- Large normal (262144) wins consistently: warm gradients 1.53–1.74x,
  source-to-Fit 1.94–1.96x. Hierarchical GP source-to-Fit improves about 4–5%.
- Radon/one chain loses source-to-Fit in all five screen rounds and all
  seven confirmation rounds: confirmed ratio 0.942 [0.924, 0.988], about
  6% slower. Eight Schools/one chain also loses all seven confirmation rounds,
  with much larger variation. Normal 1024/four chains and gamma 128/four chains
  lose six of seven confirmed source-to-Fit comparisons.
- Preparation explains much of the smaller-model total-time deficit.
  Confirmed radon preparation is 37.25 ms SYSTEM versus 43.46 ms private;
  Eight Schools is 16.60 versus 31.22 ms. Sampling alone can improve while
  the complete source-to-Fit boundary loses. Those gains do not waive startup.
- Normal 128/one worker is a smaller consistent warm loss: 301.53 versus
  308.93 ns, paired ratio 0.975 [0.948, 0.980], zero of five positive rounds.
  The identical-binary controls are much closer to parity. It did not cross
  the frozen confirmation trigger and was not resampled; this is an open
  negative, not an accepted sub-3% budget.
- Normal 8/four workers reverses from 0.821 in screening to 1.367 in
  confirmation, but its identical-private-binary controls are highly unstable.
  Normal 128/four workers also reverses with unstable controls. Neither is
  declared repaired or a reliable win.

### Diagnostic hypotheses and bounded next experiment

Read-only scope inspection confirms `stanli_embed_stanc()` adds the
`-output-complete-obj` object (including OCaml's C runtime) to the shared
target before the ELF/PE allocator rewrite. Thus the current rewrite includes
the embedded compiler object as well as Stanli/Eigen C allocations. External
dependency archives and C++ new/delete remain unchanged.

This is a **candidate explanation**, not a demonstrated cause of the cold
regression. First-use allocator initialization/page faults and host scheduling
remain alternatives. Existing-MIR preparation is a different boundary and
does not isolate them causally. The warm normal-128 loss is separate.

The next discriminating experiment would compare three matched builds:
SYSTEM, the frozen private candidate, and private allocation with the embedded
compiler object explicitly left on SYSTEM. Predeclare fixed source compilation,
existing-MIR preparation, first-gradient, source-to-Fit, warm-canary and RSS
measurements; retain the original negatives. The exclusion must be structural,
not a model/size test, and must preserve buffer ownership. This ablation is
**not implemented or measured**, and would require a new promotion evaluation
if it survives. Eliminating repeated temporaries remains a different, deferred
architecture rather than an allocator setting.

## x86_64 warm gradients: all 52 screen cells

| Model | Workers | SYSTEM ns | Private ns | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 234.89 | 230.65 | 1.013 [1.011, 1.143] | 5/5 | 1.000 [0.995, 1.085] | 1.002 [0.944, 1.013] |
| eight_schools_noncentered | 4 | 110.35 | 98.60 | 1.105 [1.095, 1.173] | 5/5 | 1.112 [1.067, 1.141] | 1.006 [0.985, 1.059] |
| eight_schools_centered | 1 | 258.52 | 252.89 | 1.032 [1.008, 1.036] | 5/5 | 1.000 [0.928, 1.022] | 1.004 [0.973, 1.028] |
| eight_schools_centered | 4 | 117.05 | 113.53 | 1.009 [0.966, 1.082] | 3/5 | 0.995 [0.959, 1.013] | 0.966 [0.951, 1.062] |
| kidscore_momiq | 1 | 1690.97 | 1658.22 | 1.022 [1.011, 1.033] | 5/5 | 0.993 [0.989, 1.005] | 1.006 [1.002, 1.013] |
| kidscore_momiq | 4 | 725.29 | 702.21 | 1.034 [0.995, 1.095] | 4/5 | 1.002 [0.952, 1.050] | 0.996 [0.976, 1.031] |
| radon_pooled | 1 | 47595.64 | 47646.79 | 0.999 [0.968, 1.010] | 1/5 | 1.000 [0.986, 1.058] | 1.007 [0.981, 1.042] |
| radon_pooled | 4 | 20707.61 | 20446.21 | 1.004 [0.947, 1.075] | 3/5 | 1.012 [0.924, 1.077] | 1.022 [1.000, 1.050] |
| logistic_regression_rhs | 1 | 57694.26 | 57842.53 | 1.000 [0.942, 1.018] | 3/5 | 0.997 [0.950, 1.021] | 0.995 [0.948, 1.064] |
| logistic_regression_rhs | 4 | 26937.08 | 26418.22 | 1.032 [0.950, 1.042] | 4/5 | 1.019 [0.973, 1.025] | 0.997 [0.978, 1.057] |
| normal_mixture | 1 | 68985.03 | 69048.47 | 0.987 [0.981, 1.094] | 2/5 | 1.001 [0.994, 1.063] | 1.011 [0.984, 1.018] |
| normal_mixture | 4 | 28850.81 | 28972.52 | 1.004 [0.970, 1.009] | 3/5 | 1.004 [0.958, 1.020] | 0.989 [0.978, 1.070] |
| garch11 | 1 | 7951.30 | 7857.30 | 1.009 [1.003, 1.015] | 5/5 | 0.985 [0.947, 1.000] | 0.997 [0.988, 1.002] |
| garch11 | 4 | 2984.86 | 2998.25 | 1.000 [0.952, 1.018] | 2/5 | 1.009 [0.961, 1.044] | 0.995 [0.975, 1.006] |
| gp_regr | 1 | 3153.02 | 3124.70 | 1.009 [1.004, 1.027] | 5/5 | 0.998 [0.990, 1.007] | 0.996 [0.986, 1.003] |
| gp_regr | 4 | 1328.05 | 1291.36 | 1.026 [1.003, 1.138] | 5/5 | 1.013 [1.012, 1.126] | 0.990 [0.889, 1.004] |
| hierarchical_gp | 1 | 26427.80 | 25285.77 | 1.045 [1.041, 1.069] | 5/5 | 0.996 [0.964, 1.027] | 1.003 [0.995, 1.028] |
| hierarchical_gp | 4 | 10688.54 | 10280.27 | 1.040 [1.038, 1.045] | 5/5 | 1.001 [0.995, 1.003] | 0.987 [0.985, 1.003] |
| hmm_example | 1 | 26283.94 | 26385.13 | 0.997 [0.959, 1.008] | 1/5 | 1.000 [0.996, 1.034] | 1.010 [1.001, 1.042] |
| hmm_example | 4 | 9891.81 | 9944.53 | 0.992 [0.961, 1.048] | 2/5 | 0.999 [0.995, 1.024] | 1.002 [0.969, 1.038] |
| lotka_volterra | 1 | 24826.67 | 24752.70 | 1.002 [0.998, 1.007] | 3/5 | 1.001 [0.997, 1.046] | 1.001 [0.996, 1.008] |
| lotka_volterra | 4 | 10988.49 | 10981.44 | 0.999 [0.962, 1.004] | 2/5 | 1.001 [0.999, 1.055] | 1.000 [0.997, 1.044] |
| soil_incubation | 1 | 34059.61 | 33551.34 | 1.021 [0.985, 1.079] | 4/5 | 1.018 [0.998, 1.104] | 0.988 [0.941, 1.064] |
| soil_incubation | 4 | 15342.24 | 14827.69 | 1.027 [0.990, 1.065] | 4/5 | 1.033 [1.002, 1.080] | 0.999 [0.944, 1.019] |
| one_comp_mm_elim_abs | 1 | 606144.08 | 604911.27 | 1.005 [0.991, 1.069] | 4/5 | 1.003 [0.950, 1.061] | 0.997 [0.975, 0.998] |
| one_comp_mm_elim_abs | 4 | 245574.61 | 244932.51 | 1.009 [0.967, 1.013] | 3/5 | 1.003 [0.974, 1.007] | 1.007 [0.996, 1.061] |
| GLM_Poisson_model | 1 | 693.30 | 678.56 | 1.017 [0.966, 1.034] | 4/5 | 1.004 [0.989, 1.015] | 1.005 [0.998, 1.053] |
| GLM_Poisson_model | 4 | 286.23 | 281.47 | 1.021 [1.002, 1.025] | 5/5 | 0.997 [0.989, 1.045] | 1.005 [0.989, 1.036] |
| dogs | 1 | 11489.07 | 11393.21 | 1.008 [0.993, 1.036] | 4/5 | 1.002 [0.992, 1.016] | 0.999 [0.995, 1.002] |
| dogs | 4 | 4846.89 | 4832.41 | 1.007 [0.997, 1.077] | 4/5 | 0.998 [0.968, 1.060] | 0.988 [0.973, 0.990] |
| diamonds | 1 | 34770.80 | 33512.38 | 1.039 [0.941, 1.047] | 4/5 | 0.998 [0.994, 1.019] | 0.947 [0.896, 1.045] |
| diamonds | 4 | 17164.25 | 16509.96 | 1.064 [0.971, 1.084] | 4/5 | 1.010 [0.941, 1.016] | 0.978 [0.949, 1.053] |
| normal_8 | 1 | 142.64 | 138.77 | 1.027 [0.991, 1.029] | 4/5 | 0.998 [0.987, 1.005] | 0.992 [0.966, 1.034] |
| normal_8 | 4 | 70.23 | 83.25 | 0.821 [0.777, 0.937] | 0/5 | 1.032 [1.009, 1.091] | 0.711 [0.659, 0.824] |
| normal_128 | 1 | 301.53 | 308.92 | 0.975 [0.948, 0.980] | 0/5 | 1.001 [0.999, 1.002] | 1.004 [0.997, 1.030] |
| normal_128 | 4 | 136.99 | 140.44 | 0.968 [0.865, 0.995] | 0/5 | 1.014 [0.936, 1.050] | 1.107 [1.091, 1.133] |
| normal_1024 | 1 | 1315.50 | 1264.48 | 1.041 [1.039, 1.068] | 5/5 | 1.001 [1.000, 1.029] | 0.998 [0.980, 0.999] |
| normal_1024 | 4 | 586.83 | 558.53 | 1.056 [1.053, 1.266] | 5/5 | 1.018 [0.662, 1.211] | 1.022 [1.008, 1.034] |
| normal_16384 | 1 | 20396.32 | 20214.69 | 1.024 [1.008, 1.035] | 5/5 | 0.941 [0.911, 1.015] | 1.003 [1.003, 1.024] |
| normal_16384 | 4 | 9307.76 | 8921.09 | 1.049 [1.034, 1.060] | 5/5 | 1.017 [0.966, 1.030] | 0.969 [0.945, 0.984] |
| normal_262144 | 1 | 1265314.88 | 729073.41 | 1.736 [1.732, 1.743] | 5/5 | 1.008 [0.998, 1.013] | 1.005 [0.998, 1.010] |
| normal_262144 | 4 | 528251.59 | 336009.70 | 1.567 [1.554, 1.589] | 5/5 | 0.982 [0.969, 0.998] | 1.007 [0.994, 1.017] |
| gamma_128 | 1 | 1728.76 | 1729.23 | 0.999 [0.999, 1.026] | 1/5 | 1.000 [0.974, 1.000] | 0.981 [0.971, 1.002] |
| gamma_128 | 4 | 728.88 | 724.64 | 1.010 [1.002, 1.060] | 5/5 | 1.004 [0.995, 1.032] | 0.998 [0.953, 1.001] |
| gamma_16384 | 1 | 200190.90 | 199963.97 | 1.000 [0.988, 1.055] | 2/5 | 1.000 [0.988, 1.010] | 0.999 [0.974, 1.000] |
| gamma_16384 | 4 | 87296.54 | 87532.77 | 0.990 [0.976, 1.004] | 2/5 | 0.991 [0.982, 1.005] | 1.002 [0.994, 1.005] |
| eight_schools_noncentered | 8 | 103.36 | 100.09 | 1.037 [1.031, 1.042] | 5/5 | 1.025 [1.020, 1.035] | 1.007 [0.984, 1.020] |
| gamma_16384 | 8 | 86579.35 | 86960.95 | 0.996 [0.981, 1.014] | 2/5 | 1.007 [0.946, 1.024] | 1.003 [0.981, 1.010] |
| hierarchical_gp | 8 | 10759.68 | 10327.99 | 1.040 [1.032, 1.041] | 5/5 | 1.001 [0.969, 1.009] | 0.991 [0.990, 1.005] |
| normal_1024 | 8 | 578.47 | 540.82 | 1.053 [1.021, 1.105] | 5/5 | 0.988 [0.940, 1.032] | 1.004 [0.958, 1.043] |
| normal_262144 | 8 | 486369.86 | 318161.21 | 1.529 [1.506, 1.558] | 5/5 | 1.009 [0.961, 1.017] | 1.006 [0.988, 1.018] |
| normal_8 | 8 | 64.13 | 63.32 | 1.004 [0.961, 1.014] | 3/5 | 1.008 [0.988, 1.034] | 0.774 [0.733, 0.790] |

## x86_64 warm gradients: fixed confirmation

| Model | Workers | SYSTEM ns | Private ns | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 235.01 | 231.48 | 1.012 [0.995, 1.079] | 6/7 | 0.996 [0.992, 1.054] | 1.003 [0.979, 1.018] |
| hierarchical_gp | 4 | 10719.08 | 10290.68 | 1.043 [1.039, 1.115] | 7/7 | 1.000 [0.965, 1.079] | 0.995 [0.938, 1.004] |
| normal_1024 | 4 | 581.74 | 558.86 | 1.043 [1.034, 1.100] | 7/7 | 1.009 [0.996, 1.067] | 1.003 [0.996, 1.017] |
| normal_128 | 4 | 143.96 | 136.56 | 1.071 [0.989, 1.087] | 6/7 | 1.068 [1.030, 1.150] | 0.947 [0.925, 1.019] |
| normal_8 | 4 | 81.04 | 60.12 | 1.367 [1.309, 1.469] | 7/7 | 0.990 [0.944, 1.074] | 0.990 [0.659, 1.046] |

## x86_64 source-to-Fit: all 14 screen cells

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 25.507 | 27.120 | 0.928 [0.399, 0.993] | 0/5 | 0.960 [0.880, 1.021] | 0.829 [0.430, 1.377] |
| eight_schools_noncentered | 4 | 52.403 | 62.074 | 0.846 [0.591, 1.032] | 2/5 | 1.002 [0.999, 1.012] | 1.183 [0.693, 1.396] |
| radon_pooled | 1 | 323.583 | 339.602 | 0.952 [0.909, 0.983] | 0/5 | 0.996 [0.829, 1.026] | 1.019 [0.906, 1.031] |
| radon_pooled | 4 | 1209.959 | 1200.719 | 0.985 [0.975, 1.034] | 2/5 | 1.024 [0.971, 1.051] | 0.994 [0.984, 1.033] |
| hierarchical_gp | 1 | 7019.673 | 6641.472 | 1.052 [1.043, 1.081] | 5/5 | 1.004 [0.990, 1.032] | 0.988 [0.948, 0.999] |
| hierarchical_gp | 4 | 32851.517 | 31661.337 | 1.037 [1.027, 1.048] | 5/5 | 1.004 [0.994, 1.014] | 1.002 [0.988, 1.008] |
| normal_1024 | 1 | 28.786 | 68.328 | 0.423 [0.402, 0.924] | 0/5 | 1.012 [0.946, 1.023] | 1.139 [0.982, 2.352] |
| normal_1024 | 4 | 62.808 | 68.682 | 0.915 [0.706, 1.121] | 2/5 | 1.012 [1.006, 1.016] | 0.873 [0.808, 1.420] |
| normal_262144 | 1 | 11004.007 | 5655.351 | 1.941 [1.596, 1.972] | 5/5 | 1.003 [0.976, 1.022] | 0.998 [0.988, 1.007] |
| normal_262144 | 4 | 44301.208 | 22535.152 | 1.961 [1.927, 1.966] | 5/5 | 1.003 [0.984, 1.236] | 1.004 [0.998, 1.015] |
| gamma_128 | 1 | 42.876 | 42.165 | 1.011 [0.513, 1.177] | 3/5 | 0.999 [0.992, 1.011] | 0.913 [0.781, 0.970] |
| gamma_128 | 4 | 112.803 | 123.856 | 0.908 [0.852, 0.945] | 0/5 | 0.997 [0.996, 1.005] | 0.903 [0.775, 1.126] |
| lotka_volterra | 1 | 1277.475 | 1311.916 | 0.978 [0.966, 1.023] | 2/5 | 1.002 [0.974, 1.009] | 1.018 [0.988, 1.034] |
| lotka_volterra | 4 | 5284.109 | 5284.259 | 1.004 [0.993, 1.021] | 3/5 | 1.008 [0.975, 1.030] | 0.977 [0.947, 0.991] |

## x86_64 sampling alone: all 14 screen cells

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 8.828 | 7.349 | 1.197 [0.980, 1.481] | 4/5 | 0.985 [0.869, 1.003] | 1.049 [0.744, 1.323] |
| eight_schools_noncentered | 4 | 35.711 | 31.047 | 1.150 [1.022, 1.650] | 5/5 | 1.003 [0.985, 1.010] | 0.831 [0.749, 1.407] |
| radon_pooled | 1 | 286.138 | 286.385 | 1.010 [0.983, 1.023] | 3/5 | 0.995 [0.827, 1.030] | 0.997 [0.917, 1.007] |
| radon_pooled | 4 | 1171.611 | 1137.450 | 1.007 [0.974, 1.040] | 4/5 | 1.022 [0.970, 1.052] | 1.001 [0.990, 1.057] |
| hierarchical_gp | 1 | 6960.556 | 6545.033 | 1.059 [1.049, 1.080] | 5/5 | 1.004 [0.990, 1.031] | 0.990 [0.952, 0.999] |
| hierarchical_gp | 4 | 32792.491 | 31583.649 | 1.038 [1.028, 1.048] | 5/5 | 1.004 [0.994, 1.014] | 1.002 [0.987, 1.008] |
| normal_1024 | 1 | 13.264 | 11.680 | 1.134 [0.887, 1.266] | 3/5 | 1.011 [0.914, 1.013] | 1.177 [0.999, 1.590] |
| normal_1024 | 4 | 47.044 | 39.897 | 1.194 [0.980, 1.203] | 4/5 | 1.012 [1.010, 1.027] | 1.180 [0.935, 1.343] |
| normal_262144 | 1 | 10900.011 | 5535.916 | 1.971 [1.612, 1.991] | 5/5 | 1.003 [0.976, 1.022] | 1.000 [0.990, 1.003] |
| normal_262144 | 4 | 44197.105 | 22398.705 | 1.966 [1.935, 1.973] | 5/5 | 1.003 [0.984, 1.236] | 1.003 [0.997, 1.015] |
| gamma_128 | 1 | 27.337 | 21.587 | 1.267 [1.065, 1.288] | 5/5 | 1.001 [0.996, 1.005] | 0.941 [0.789, 0.999] |
| gamma_128 | 4 | 97.708 | 91.521 | 1.067 [0.943, 1.084] | 4/5 | 1.000 [0.998, 1.002] | 0.966 [0.782, 1.218] |
| lotka_volterra | 1 | 1255.439 | 1249.626 | 1.006 [1.003, 1.038] | 5/5 | 1.003 [0.973, 1.007] | 0.993 [0.973, 0.999] |
| lotka_volterra | 4 | 5262.400 | 5256.141 | 1.007 [0.993, 1.021] | 4/5 | 1.008 [0.975, 1.030] | 0.981 [0.954, 0.998] |

## x86_64 startup and peak memory: screen

| Model | Chains | Prepare SYSTEM/private ms | Import SYSTEM/private ms | Peak RSS SYSTEM/private MiB |
|---|---:|---|---|---|
| eight_schools_noncentered | 1 | 16.679 / 19.770 | 55.355 / 55.767 | 84.70 / 84.70 |
| eight_schools_noncentered | 4 | 16.618 / 26.680 | 55.354 / 56.227 | 84.70 / 84.70 |
| radon_pooled | 1 | 37.355 / 56.807 | 55.693 / 59.661 | 84.70 / 84.70 |
| radon_pooled | 4 | 37.315 / 54.111 | 55.665 / 57.050 | 84.70 / 84.70 |
| hierarchical_gp | 1 | 59.117 / 97.109 | 55.573 / 58.960 | 95.54 / 114.16 |
| hierarchical_gp | 4 | 59.131 / 76.173 | 56.043 / 67.837 | 173.59 / 193.66 |
| normal_1024 | 1 | 15.522 / 56.221 | 55.081 / 54.771 | 84.70 / 84.70 |
| normal_1024 | 4 | 15.662 / 20.515 | 55.548 / 58.322 | 82.49 / 82.49 |
| normal_262144 | 1 | 103.996 / 117.208 | 55.780 / 56.450 | 83.72 / 102.03 |
| normal_262144 | 4 | 104.103 / 136.447 | 56.234 / 56.165 | 89.68 / 105.80 |
| gamma_128 | 1 | 15.528 / 17.415 | 55.605 / 101.754 | 84.70 / 84.70 |
| gamma_128 | 4 | 15.096 / 32.335 | 54.845 / 56.735 | 84.70 / 84.70 |
| lotka_volterra | 1 | 22.001 / 60.589 | 55.407 / 57.012 | 84.70 / 84.70 |
| lotka_volterra | 4 | 21.709 / 25.418 | 56.019 / 59.162 | 84.70 / 84.70 |

## x86_64 source-to-Fit: fixed confirmation

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 25.415 | 48.018 | 0.564 [0.461, 0.899] | 0/7 | 1.000 [0.990, 1.021] | 1.288 [0.694, 2.193] |
| eight_schools_noncentered | 4 | 52.696 | 52.022 | 1.008 [0.680, 1.374] | 4/7 | 1.004 [0.985, 1.032] | 0.990 [0.765, 1.536] |
| gamma_128 | 4 | 113.056 | 128.607 | 0.889 [0.781, 1.030] | 1/7 | 1.001 [0.989, 1.108] | 1.063 [0.897, 1.121] |
| normal_1024 | 1 | 28.753 | 59.428 | 0.529 [0.448, 1.051] | 3/7 | 0.982 [0.937, 1.032] | 0.987 [0.811, 1.809] |
| normal_1024 | 4 | 62.356 | 73.970 | 0.866 [0.719, 1.090] | 1/7 | 1.001 [0.901, 1.018] | 1.093 [0.904, 1.258] |
| radon_pooled | 1 | 325.006 | 346.584 | 0.942 [0.924, 0.988] | 0/7 | 1.002 [0.988, 1.007] | 1.020 [0.952, 1.095] |

## x86_64 sampling alone: fixed confirmation

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 8.835 | 8.238 | 1.073 [0.556, 1.666] | 6/7 | 1.001 [0.996, 1.004] | 1.120 [0.555, 1.912] |
| eight_schools_noncentered | 4 | 35.489 | 30.179 | 1.186 [1.016, 1.631] | 7/7 | 0.997 [0.982, 1.029] | 0.938 [0.714, 1.199] |
| gamma_128 | 4 | 98.052 | 98.342 | 1.042 [0.992, 1.137] | 6/7 | 1.007 [0.997, 1.113] | 0.979 [0.847, 1.114] |
| normal_1024 | 1 | 13.134 | 11.050 | 1.217 [1.001, 1.479] | 7/7 | 0.997 [0.943, 1.016] | 0.811 [0.639, 1.001] |
| normal_1024 | 4 | 46.446 | 55.050 | 0.851 [0.753, 1.268] | 3/7 | 0.994 [0.909, 1.008] | 1.040 [0.911, 1.149] |
| radon_pooled | 1 | 287.947 | 295.397 | 0.986 [0.921, 1.008] | 1/7 | 1.003 [0.993, 1.008] | 1.030 [0.989, 1.130] |

## x86_64 startup and peak memory: confirmation

| Model | Chains | Prepare SYSTEM/private ms | Import SYSTEM/private ms | Peak RSS SYSTEM/private MiB |
|---|---:|---|---|---|
| eight_schools_noncentered | 1 | 16.602 / 31.222 | 56.005 / 62.531 | 63.57 / 75.66 |
| eight_schools_noncentered | 4 | 17.134 / 20.882 | 57.231 / 57.286 | 63.57 / 76.18 |
| gamma_128 | 4 | 15.463 / 30.265 | 56.007 / 56.691 | 63.57 / 75.31 |
| normal_1024 | 1 | 15.545 / 48.254 | 55.896 / 56.226 | 63.57 / 75.06 |
| normal_1024 | 4 | 15.745 / 22.965 | 56.455 / 56.737 | 63.57 / 75.35 |
| radon_pooled | 1 | 37.246 / 43.462 | 57.056 / 56.591 | 63.57 / 78.02 |

## ARM64 correctness, ownership and memory

ARM64 also **does not pass the default-promotion gate**. Its
[full artifact](https://github.com/seantalts/stanli/actions/runs/34758730953/artifacts/10320536135)
uses the same frozen source/protocol, GCC 14.2.1, manylinux glibc 2.28 and
Python 3.12/NumPy 2.5.3. The four-CPU host reports ARM implementer
0x41, architecture 8, part 0xd49. There is no CPU quota or BLAS thread-variable
override; 8-worker measurements oversubscribe the host.

Independent artifact auditing passes all four library hashes, model hashes,
2,688 gradient snapshots and graph dimensions, 392 Fit snapshots, and
72 retention snapshots. Every worker's recorded warmup reaches 500 ms.
SYSTEM CTest passes 246/246 and private CTest 250/250, with both Python suites
passing before measurement. There are 2,080 gradient screen processes and
504 fixed-confirmation processes (nine cells), plus 280 sampling screen
and 84 confirmation processes (three cells). No failed numerical sample is
discarded, and all collection commands return zero.

| Retention metric | SYSTEM MiB | Private MiB |
|---|---:|---:|
| Initial RSS | 34.84 | 37.46 |
| Peak RSS | 85.11 | 97.17 |
| Final destroyed-model RSS | 39.94 | 75.36 |
| Destroyed-model range, last six cycles | 39.93–43.95 | 51.42–79.37 |

The private process alternates between lower and higher retained plateaus;
it does not grow without bound during these 12 sequences, but ends about
35.4 MiB higher than SYSTEM. This bounded result is not a leak-proof guarantee
or automatic acceptance of the memory cost.

## ARM64 interpretation

- Large-normal warm gradients improve 3.89–4.13x across 1/4/8 workers.
  Complete source-to-Fit improves 2.87–2.90x at one/four chains; every screen
  round is positive. Eight Schools also improves consistently in the screen:
  about 1.09–1.13x source-to-Fit and 1.28–1.29x sampling alone.
- HMM loses all five screen and all seven confirmation rounds at both
  one and four workers. Confirmed ratios are 0.969 [0.964, 0.972] and
  0.968 [0.965, 0.970]: about 3.2–3.3% slower, with A/A controls near parity.
  This directly blocks promotion.
- Diamonds also loses all seven confirmed rounds: one worker 0.959
  [0.945, 0.964], four workers 0.894 [0.878, 0.930], about 4.3% and 11.9%
  slower. Screening had substantial private A/A instability; confirmation
  reduces that instability but does not remove the negative result.
- Gamma 128/four-worker warm gradients return near parity in confirmation.
  Normal 8/four workers also has a near-parity candidate ratio, but its
  private A/A median is 0.896. This remains measurement instability, not a fix.
- Normal mixture/one worker is a smaller consistent negative: ratio 0.999,
  all five rounds below one, with a minimum 0.997. Its roughly 0.1% median
  difference is at the scale of A/A variation and is inconclusive; it was
  not retried under a new threshold.
- Gamma 128/one-chain source-to-Fit remains noisy: confirmed ratio 0.966
  [0.877, 1.060], only three of seven positive, with private A/A spanning
  0.917–1.201. Sampling alone is consistently about 1.068x faster; preparation
  is 15.93 ms SYSTEM versus 19.53 ms private. The total-time signal remains
  unresolved, not a clean gain or proof of a repeatable 3.5% loss.
- Normal 1024/one-chain source-to-Fit screens slightly negative with a wide
  range (0.991 [0.836, 1.057]); no additional confirmation was triggered.
  Every other cell, including mixed/noisy negatives, is retained below.

The ARM64 warm regressions run from frozen existing MIR after sustained warmup.
Excluding the embedded compiler alone cannot explain or establish a repair for
them. A separate bounded allocation/profile ablation of HMM/diamonds and a
positive canary is needed to distinguish allocator-call cost from
memory-placement effects. Neither new production special cases nor allocator
tuning were added after seeing these results.

## ARM64 warm gradients: all 52 screen cells

| Model | Workers | SYSTEM ns | Private ns | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 323.06 | 292.93 | 1.106 [1.100, 1.111] | 5/5 | 1.001 [0.997, 1.002] | 1.001 [0.999, 1.002] |
| eight_schools_noncentered | 4 | 81.49 | 75.20 | 1.084 [1.075, 1.094] | 5/5 | 1.001 [0.999, 1.010] | 1.003 [0.996, 1.004] |
| eight_schools_centered | 1 | 353.27 | 314.13 | 1.129 [1.125, 1.130] | 5/5 | 1.002 [1.001, 1.005] | 1.001 [0.997, 1.001] |
| eight_schools_centered | 4 | 90.33 | 88.74 | 1.023 [1.015, 1.027] | 5/5 | 0.915 [0.907, 0.929] | 1.100 [1.096, 1.109] |
| kidscore_momiq | 1 | 2212.92 | 2133.42 | 1.035 [1.033, 1.040] | 5/5 | 0.997 [0.991, 1.006] | 1.002 [0.997, 1.006] |
| kidscore_momiq | 4 | 564.23 | 544.62 | 1.035 [1.024, 1.043] | 5/5 | 1.006 [0.992, 1.015] | 1.014 [0.994, 1.022] |
| radon_pooled | 1 | 59904.48 | 59927.78 | 1.000 [0.992, 1.011] | 2/5 | 0.996 [0.988, 1.003] | 1.005 [0.990, 1.014] |
| radon_pooled | 4 | 17174.88 | 17266.89 | 0.987 [0.983, 1.017] | 2/5 | 0.984 [0.973, 0.995] | 1.000 [0.997, 1.015] |
| logistic_regression_rhs | 1 | 102055.91 | 101857.48 | 1.000 [0.986, 1.012] | 2/5 | 1.002 [0.998, 1.007] | 1.009 [0.994, 1.012] |
| logistic_regression_rhs | 4 | 31080.87 | 31180.72 | 1.008 [0.976, 1.025] | 3/5 | 1.004 [0.989, 1.028] | 0.995 [0.985, 1.014] |
| normal_mixture | 1 | 90631.77 | 90734.17 | 0.999 [0.997, 1.000] | 0/5 | 1.000 [1.000, 1.001] | 1.001 [1.000, 1.002] |
| normal_mixture | 4 | 22762.41 | 22773.67 | 1.000 [0.999, 1.005] | 2/5 | 1.001 [0.995, 1.005] | 0.998 [0.995, 1.000] |
| garch11 | 1 | 9425.17 | 9320.64 | 1.011 [1.011, 1.012] | 5/5 | 0.997 [0.996, 0.998] | 0.998 [0.998, 0.999] |
| garch11 | 4 | 2391.86 | 2365.91 | 1.011 [1.010, 1.015] | 5/5 | 0.996 [0.957, 1.001] | 0.995 [0.993, 0.999] |
| gp_regr | 1 | 3802.10 | 3656.30 | 1.040 [1.038, 1.040] | 5/5 | 1.000 [1.000, 1.001] | 1.000 [0.998, 1.001] |
| gp_regr | 4 | 954.72 | 918.30 | 1.040 [1.039, 1.044] | 5/5 | 1.000 [0.995, 1.005] | 0.998 [0.996, 1.000] |
| hierarchical_gp | 1 | 33229.99 | 31549.70 | 1.051 [1.038, 1.062] | 5/5 | 0.994 [0.988, 1.000] | 0.997 [0.986, 1.013] |
| hierarchical_gp | 4 | 8407.89 | 8084.88 | 1.036 [1.002, 1.054] | 5/5 | 0.992 [0.988, 1.011] | 0.998 [0.976, 1.037] |
| hmm_example | 1 | 31971.23 | 33073.81 | 0.964 [0.962, 0.970] | 0/5 | 1.002 [1.000, 1.003] | 1.002 [0.998, 1.007] |
| hmm_example | 4 | 8020.57 | 8312.40 | 0.964 [0.960, 0.994] | 0/5 | 0.999 [0.995, 1.029] | 1.002 [0.995, 1.009] |
| lotka_volterra | 1 | 37513.87 | 36933.64 | 1.015 [1.013, 1.018] | 5/5 | 1.001 [0.996, 1.003] | 0.999 [0.997, 0.999] |
| lotka_volterra | 4 | 9501.12 | 9343.94 | 1.016 [1.010, 1.023] | 5/5 | 1.002 [1.000, 1.008] | 0.995 [0.994, 1.007] |
| soil_incubation | 1 | 48730.01 | 48405.16 | 1.007 [1.004, 1.007] | 5/5 | 1.000 [0.998, 1.002] | 0.999 [0.998, 1.000] |
| soil_incubation | 4 | 12256.28 | 12194.52 | 1.005 [0.997, 1.008] | 4/5 | 1.001 [0.991, 1.004] | 1.001 [0.991, 1.006] |
| one_comp_mm_elim_abs | 1 | 759914.23 | 741433.86 | 1.024 [1.021, 1.025] | 5/5 | 1.001 [0.999, 1.006] | 1.000 [0.998, 1.005] |
| one_comp_mm_elim_abs | 4 | 190344.22 | 186294.39 | 1.022 [1.018, 1.026] | 5/5 | 0.999 [0.992, 1.002] | 1.000 [0.993, 1.001] |
| GLM_Poisson_model | 1 | 819.37 | 809.59 | 1.011 [1.010, 1.014] | 5/5 | 1.002 [1.000, 1.004] | 0.999 [0.999, 1.001] |
| GLM_Poisson_model | 4 | 206.66 | 204.29 | 1.013 [1.008, 1.016] | 5/5 | 1.008 [1.004, 1.012] | 1.001 [0.999, 1.005] |
| dogs | 1 | 16299.66 | 16300.39 | 1.001 [0.999, 1.003] | 4/5 | 0.998 [0.995, 1.001] | 1.001 [1.000, 1.003] |
| dogs | 4 | 4121.76 | 4112.52 | 1.003 [0.999, 1.004] | 4/5 | 0.999 [0.986, 0.999] | 0.998 [0.998, 0.999] |
| diamonds | 1 | 58469.73 | 64327.99 | 0.907 [0.901, 0.913] | 0/5 | 1.000 [0.991, 1.011] | 1.098 [1.056, 1.105] |
| diamonds | 4 | 19375.13 | 23894.79 | 0.802 [0.794, 0.841] | 0/5 | 0.988 [0.967, 1.007] | 1.206 [1.203, 1.218] |
| normal_8 | 1 | 183.20 | 168.96 | 1.084 [1.083, 1.085] | 5/5 | 1.000 [0.998, 1.001] | 0.998 [0.998, 0.999] |
| normal_8 | 4 | 50.87 | 52.86 | 0.960 [0.956, 0.967] | 0/5 | 1.009 [0.970, 1.031] | 1.198 [1.190, 1.205] |
| normal_128 | 1 | 372.24 | 368.06 | 1.012 [1.011, 1.013] | 5/5 | 1.000 [0.999, 1.001] | 1.000 [0.998, 1.001] |
| normal_128 | 4 | 105.63 | 95.48 | 1.113 [1.102, 1.118] | 5/5 | 1.124 [1.116, 1.126] | 0.928 [0.923, 0.952] |
| normal_1024 | 1 | 1908.33 | 1843.13 | 1.035 [1.033, 1.042] | 5/5 | 1.004 [0.998, 1.019] | 0.996 [0.990, 0.997] |
| normal_1024 | 4 | 489.87 | 473.38 | 1.034 [1.033, 1.045] | 5/5 | 1.018 [1.014, 1.029] | 1.005 [0.990, 1.010] |
| normal_16384 | 1 | 27389.12 | 27016.27 | 1.018 [1.005, 1.021] | 5/5 | 1.004 [0.994, 1.018] | 1.000 [0.997, 1.006] |
| normal_16384 | 4 | 7111.76 | 6790.10 | 1.058 [1.029, 1.071] | 5/5 | 1.020 [0.939, 1.032] | 1.000 [0.998, 1.002] |
| normal_262144 | 1 | 1821873.50 | 469433.33 | 3.886 [3.850, 3.899] | 5/5 | 1.003 [0.996, 1.010] | 0.998 [0.997, 1.007] |
| normal_262144 | 4 | 523277.60 | 126628.20 | 4.131 [4.072, 4.191] | 5/5 | 1.004 [0.968, 1.016] | 0.997 [0.986, 1.022] |
| gamma_128 | 1 | 2194.29 | 2194.79 | 1.000 [0.985, 1.002] | 2/5 | 1.000 [0.999, 1.002] | 1.000 [0.999, 1.015] |
| gamma_128 | 4 | 561.27 | 582.41 | 0.964 [0.960, 0.966] | 0/5 | 0.987 [0.982, 0.994] | 1.000 [0.998, 1.004] |
| gamma_16384 | 1 | 257681.94 | 257684.41 | 1.000 [0.999, 1.001] | 3/5 | 1.000 [1.000, 1.001] | 1.000 [1.000, 1.000] |
| gamma_16384 | 4 | 64685.29 | 64683.27 | 0.999 [0.996, 1.001] | 1/5 | 1.000 [0.989, 1.002] | 1.001 [0.998, 1.002] |
| eight_schools_noncentered | 8 | 81.40 | 74.90 | 1.096 [1.017, 1.136] | 5/5 | 0.999 [0.971, 1.077] | 0.978 [0.929, 1.031] |
| gamma_16384 | 8 | 65622.33 | 65314.25 | 0.996 [0.973, 1.036] | 2/5 | 0.995 [0.960, 1.033] | 0.979 [0.951, 1.023] |
| hierarchical_gp | 8 | 8681.80 | 8128.67 | 1.050 [1.027, 1.077] | 5/5 | 1.023 [0.992, 1.033] | 0.970 [0.939, 1.008] |
| normal_1024 | 8 | 499.18 | 470.32 | 1.034 [1.024, 1.101] | 5/5 | 1.014 [0.976, 1.046] | 0.995 [0.993, 0.997] |
| normal_262144 | 8 | 523396.75 | 133243.13 | 3.978 [3.864, 4.159] | 5/5 | 1.002 [0.973, 1.022] | 1.000 [0.982, 1.020] |
| normal_8 | 8 | 51.98 | 46.73 | 1.120 [1.094, 1.154] | 5/5 | 1.019 [0.986, 1.043] | 1.026 [0.971, 1.054] |

## ARM64 warm gradients: fixed confirmation

| Model | Workers | SYSTEM ns | Private ns | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| diamonds | 1 | 60012.95 | 62934.27 | 0.959 [0.945, 0.964] | 0/7 | 1.003 [0.996, 1.007] | 0.983 [0.974, 0.988] |
| diamonds | 4 | 20306.15 | 22496.91 | 0.894 [0.878, 0.930] | 0/7 | 0.995 [0.979, 1.013] | 0.993 [0.963, 1.040] |
| eight_schools_noncentered | 1 | 323.68 | 292.62 | 1.106 [1.104, 1.109] | 7/7 | 1.001 [0.998, 1.004] | 0.999 [0.998, 1.002] |
| gamma_128 | 4 | 561.65 | 561.37 | 1.000 [0.996, 1.002] | 3/7 | 1.000 [0.999, 1.006] | 1.000 [0.996, 1.003] |
| hierarchical_gp | 4 | 8414.93 | 8091.70 | 1.032 [1.013, 1.051] | 7/7 | 0.989 [0.965, 1.005] | 0.982 [0.965, 1.020] |
| hmm_example | 1 | 31964.11 | 33017.14 | 0.969 [0.964, 0.972] | 0/7 | 1.003 [0.995, 1.012] | 1.001 [0.999, 1.002] |
| hmm_example | 4 | 8031.98 | 8298.47 | 0.968 [0.965, 0.970] | 0/7 | 1.000 [0.997, 1.004] | 0.999 [0.994, 1.004] |
| normal_1024 | 4 | 488.79 | 471.76 | 1.038 [1.030, 1.043] | 7/7 | 0.997 [0.986, 1.003] | 0.989 [0.985, 0.998] |
| normal_8 | 4 | 47.17 | 47.08 | 1.000 [0.993, 1.007] | 3/7 | 0.998 [0.994, 1.001] | 0.896 [0.889, 0.912] |

## ARM64 source-to-Fit: all 14 screen cells

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 28.425 | 26.113 | 1.091 [1.053, 1.121] | 5/5 | 0.988 [0.972, 1.016] | 0.919 [0.797, 1.068] |
| eight_schools_noncentered | 4 | 54.222 | 48.001 | 1.126 [1.100, 1.189] | 5/5 | 1.002 [0.965, 1.024] | 1.058 [1.010, 1.069] |
| radon_pooled | 1 | 405.919 | 401.734 | 1.010 [0.995, 1.023] | 4/5 | 0.998 [0.992, 1.006] | 0.996 [0.987, 1.008] |
| radon_pooled | 4 | 1512.945 | 1492.568 | 1.014 [1.009, 1.018] | 5/5 | 1.006 [0.997, 1.006] | 1.000 [0.999, 1.010] |
| hierarchical_gp | 1 | 9735.676 | 9452.652 | 1.009 [0.992, 1.044] | 3/5 | 1.005 [0.958, 1.007] | 0.999 [0.987, 1.031] |
| hierarchical_gp | 4 | 41416.578 | 40314.957 | 1.025 [1.019, 1.035] | 5/5 | 1.010 [0.999, 1.011] | 1.000 [0.995, 1.003] |
| normal_1024 | 1 | 30.503 | 30.793 | 0.991 [0.836, 1.057] | 2/5 | 0.988 [0.978, 1.014] | 1.031 [0.837, 1.088] |
| normal_1024 | 4 | 71.179 | 65.510 | 1.091 [1.047, 1.129] | 5/5 | 1.005 [0.994, 1.007] | 0.984 [0.917, 1.021] |
| normal_262144 | 1 | 10641.120 | 3710.991 | 2.867 [2.831, 2.922] | 5/5 | 0.998 [0.997, 1.012] | 1.001 [0.994, 1.006] |
| normal_262144 | 4 | 42876.913 | 14903.152 | 2.904 [2.877, 2.914] | 5/5 | 0.988 [0.982, 1.006] | 0.998 [0.992, 1.001] |
| gamma_128 | 1 | 43.414 | 44.945 | 0.966 [0.891, 1.057] | 1/5 | 1.003 [0.984, 1.007] | 1.028 [0.937, 1.111] |
| gamma_128 | 4 | 128.571 | 121.938 | 1.050 [1.042, 1.067] | 5/5 | 1.000 [0.996, 1.000] | 1.001 [0.964, 1.006] |
| lotka_volterra | 1 | 1796.493 | 1789.147 | 1.004 [0.999, 1.012] | 3/5 | 1.002 [1.000, 1.002] | 1.004 [0.996, 1.012] |
| lotka_volterra | 4 | 7650.531 | 7617.493 | 1.005 [1.003, 1.015] | 5/5 | 1.001 [0.997, 1.005] | 1.002 [0.997, 1.006] |

## ARM64 sampling alone: all 14 screen cells

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 10.435 | 8.167 | 1.281 [1.233, 1.287] | 5/5 | 0.991 [0.987, 1.011] | 0.999 [0.889, 1.036] |
| eight_schools_noncentered | 4 | 36.579 | 28.242 | 1.293 [1.250, 1.303] | 5/5 | 1.007 [0.997, 1.012] | 1.003 [0.997, 1.041] |
| radon_pooled | 1 | 367.199 | 361.113 | 1.016 [1.009, 1.027] | 5/5 | 0.997 [0.991, 1.008] | 1.001 [0.990, 1.005] |
| radon_pooled | 4 | 1473.148 | 1452.849 | 1.015 [1.009, 1.018] | 5/5 | 1.005 [0.997, 1.006] | 1.003 [0.999, 1.011] |
| hierarchical_gp | 1 | 9670.848 | 9378.461 | 1.010 [0.992, 1.044] | 3/5 | 1.005 [0.958, 1.007] | 0.999 [0.987, 1.031] |
| hierarchical_gp | 4 | 41348.960 | 40240.382 | 1.026 [1.020, 1.035] | 5/5 | 1.010 [0.999, 1.011] | 1.000 [0.995, 1.003] |
| normal_1024 | 1 | 14.583 | 12.616 | 1.154 [1.095, 1.165] | 5/5 | 0.998 [0.996, 1.002] | 1.006 [0.995, 1.061] |
| normal_1024 | 4 | 54.841 | 47.967 | 1.143 [1.126, 1.166] | 5/5 | 0.996 [0.994, 1.002] | 0.998 [0.992, 1.022] |
| normal_262144 | 1 | 10521.604 | 3592.052 | 2.929 [2.892, 2.985] | 5/5 | 0.998 [0.997, 1.012] | 1.002 [0.994, 1.006] |
| normal_262144 | 4 | 42755.552 | 14782.180 | 2.920 [2.892, 2.930] | 5/5 | 0.987 [0.982, 1.006] | 0.998 [0.992, 1.001] |
| gamma_128 | 1 | 27.256 | 25.477 | 1.069 [1.066, 1.072] | 5/5 | 1.001 [0.998, 1.004] | 0.999 [0.994, 1.002] |
| gamma_128 | 4 | 112.279 | 105.126 | 1.068 [1.063, 1.069] | 5/5 | 0.998 [0.996, 1.001] | 0.999 [0.995, 1.004] |
| lotka_volterra | 1 | 1773.431 | 1766.400 | 1.004 [1.001, 1.013] | 5/5 | 1.002 [1.000, 1.002] | 1.001 [0.997, 1.011] |
| lotka_volterra | 4 | 7627.443 | 7592.318 | 1.006 [1.003, 1.015] | 5/5 | 1.001 [0.997, 1.005] | 1.002 [0.998, 1.006] |

## ARM64 startup and peak memory: screen

| Model | Chains | Prepare SYSTEM/private ms | Import SYSTEM/private ms | Peak RSS SYSTEM/private MiB |
|---|---:|---|---|---|
| eight_schools_noncentered | 1 | 17.991 / 17.744 | 66.850 / 67.182 | 83.71 / 83.71 |
| eight_schools_noncentered | 4 | 17.598 / 18.783 | 62.326 / 65.576 | 83.71 / 83.71 |
| radon_pooled | 1 | 39.217 / 40.496 | 62.550 / 69.502 | 83.71 / 83.71 |
| radon_pooled | 4 | 39.693 / 39.183 | 65.389 / 70.631 | 83.71 / 83.71 |
| hierarchical_gp | 1 | 65.312 / 67.623 | 62.209 / 68.253 | 94.06 / 111.75 |
| hierarchical_gp | 4 | 66.038 / 68.446 | 64.182 / 70.295 | 172.11 / 192.19 |
| normal_1024 | 1 | 15.972 / 18.207 | 61.497 / 66.141 | 83.71 / 83.71 |
| normal_1024 | 4 | 16.338 / 16.948 | 64.663 / 67.038 | 81.59 / 81.59 |
| normal_262144 | 1 | 120.107 / 119.398 | 63.236 / 73.525 | 82.60 / 101.30 |
| normal_262144 | 4 | 121.361 / 120.722 | 64.058 / 69.950 | 88.53 / 105.31 |
| gamma_128 | 1 | 16.159 / 19.374 | 63.397 / 67.070 | 83.71 / 83.71 |
| gamma_128 | 4 | 16.123 / 16.925 | 65.127 / 67.589 | 83.71 / 83.71 |
| lotka_volterra | 1 | 23.062 / 24.483 | 62.983 / 65.776 | 83.71 / 83.71 |
| lotka_volterra | 4 | 23.102 / 24.339 | 63.408 / 70.064 | 83.71 / 83.71 |

## ARM64 source-to-Fit: fixed confirmation

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 28.189 | 26.141 | 1.087 [0.987, 1.142] | 6/7 | 1.020 [0.977, 1.063] | 0.987 [0.769, 1.065] |
| gamma_128 | 1 | 43.295 | 44.973 | 0.966 [0.877, 1.060] | 3/7 | 1.000 [0.982, 1.015] | 1.009 [0.917, 1.201] |
| normal_1024 | 4 | 70.924 | 63.703 | 1.114 [1.077, 1.127] | 7/7 | 0.999 [0.992, 1.009] | 0.999 [0.986, 1.023] |

## ARM64 sampling alone: fixed confirmation

| Model | Chains | SYSTEM ms | Private ms | SYSTEM/private [range] | Positive rounds | SYSTEM A/A [range] | Private A/A [range] |
|---|---:|---:|---:|---|---:|---|---|
| eight_schools_noncentered | 1 | 10.361 | 8.075 | 1.284 [1.257, 1.350] | 7/7 | 0.994 [0.988, 1.067] | 1.003 [0.986, 1.017] |
| gamma_128 | 1 | 27.228 | 25.487 | 1.068 [1.063, 1.084] | 7/7 | 1.002 [0.982, 1.015] | 1.000 [0.966, 1.005] |
| normal_1024 | 4 | 54.961 | 47.214 | 1.165 [1.158, 1.173] | 7/7 | 0.999 [0.996, 1.006] | 0.996 [0.975, 1.004] |

## ARM64 startup and peak memory: confirmation

| Model | Chains | Prepare SYSTEM/private ms | Import SYSTEM/private ms | Peak RSS SYSTEM/private MiB |
|---|---:|---|---|---|
| eight_schools_noncentered | 1 | 17.816 / 17.903 | 65.562 / 63.780 | 56.27 / 68.27 |
| gamma_128 | 1 | 15.925 / 19.527 | 64.190 / 64.352 | 55.95 / 74.44 |
| normal_1024 | 4 | 15.920 / 16.379 | 63.047 / 63.742 | 55.84 / 74.53 |


## Integration validation

- [Rebased PR CI](https://github.com/seantalts/stanli/actions/runs/34764265233)
  passed, including native, wheel/Python, corpus/BridgeStan, R and all six
  allocator ownership jobs.
- [Rebased full platform run](https://github.com/seantalts/stanli/actions/runs/34764263000)
  passed both Linux releases, both macOS architectures, Windows, R, browser,
  webR and standalone ownership jobs. ASan passes 245/245, including the
  relocated compiler deployment test; TSan passes all three threaded tests.
  The complete full-platform run is green. Sanitizer
  builds select SYSTEM by design: these validate fallback integration, not
  private mimalloc running under sanitizers.
- Post-rebase local private CTest: 250/250. The matched measured-source local
  SYSTEM build passed 246/246. Local Python passed both allocators; R passed
  173 checks per allocator with no failures/warnings/skips, plus the private
  portable-compiler integration check. These are compatibility checks, not
  Linux timing claims.
- The original measured-source ASan run is retained with its failure:
  244/245 passed; only `test_compiler_deployment` failed because the moved
  installation could not find `libstanli_core.so`. The rebased ASan run
  above passes that same test after main's installation fix. This is an
  infrastructure/packaging failure, not a discarded allocator timing sample
  or an ASan memory-error report. Both runs are linked, not pooled.

## Reproduction

With the pinned dependencies/compiler installed and an ordinary shipping
Linux Release build configured in `build-rel`, install NumPy in an isolated
Python environment, then run:

```sh
python tools/allocator/rollout.py --build build-rel --pdb deps/posteriordb \
  --output allocator-rollout --jobs 2
```

The driver refuses an existing output directory, verifies the fresh Linux
AUTO default, builds/tests both modes before timing, freezes identities and
calibration, checks all bytes, and runs exactly one diagnostic confirmation.
The workflow's `allocator_rollout=true`, `shared_allocator=DEFAULT` manual
inputs run this protocol on both shipping manylinux architectures.
A successful `completion.json` means collection completed, not acceptance.
