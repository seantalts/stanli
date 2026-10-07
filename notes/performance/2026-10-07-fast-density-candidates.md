# Fast-mode density candidates: free reduction order, FMA contraction, skipped checks

Status: measurement record, 2026-10-07, branch `scratch/fast-density-variants`
(from origin/main `1ebe01c5`). The variant code is measurement code and stays on
the scratch branch; nothing here changes the default build. The candidates feed
the fast mode in [numerics-vs-speed](2026-10-05-numerics-vs-speed.md); the draft
table rows are in "Candidates table rows" below, for pasting there later.

## Decision

- V1, free reduction order in the fused density sums: no on its own. The gain is
  1.010x over the 196 vector-argument models (1.012x by cycles) and nothing on the
  122 scalar-only ones. It is accurate (no worse than the packet order against a
  70-digit reference, usually better for the log density) but leaves the 10-ULP
  `s2_s_by` fixture gate. Take it only if a fast-kernel template exists anyway.
- V2, `-ffp-contract=fast` for `density_fused.cpp`: no. 1.000x over 317 models, 149
  corpus points change, accuracy against the truth does not move.
- V3, skipping the argument predicates: no as a fast-mode switch, because it removes
  rejections (`s2_nlf` points that CmdStan rejects return lp -inf). Its ceiling is
  large (1.047x over vector-argument models, 1.10x above 256 elements, 1.30x on
  `radon_county`), and half of it comes back with no numerics change from one
  vectorized pass in front of the same checks (V3b, 1.035x over the 137 models with
  an argument of 16 or more, byte-identical output). V3b belongs in the default
  path, not in fast mode.

## Baseline and method

macOS 26 arm64 (Apple M3 Ultra), Apple clang 21.0.0, Eigen 5.0.1, Release (`-O3
-DNDEBUG`, `-ffp-contract=off`), `-DSTANLI_STAN_DENSITY_ORACLE=OFF`, one build tree
(`build-fd`) reconfigured between arms. Binaries are frozen as copies. Speedups are
old time over new time, so above 1 is faster.

| arm | what | `bench_grad` sha256 | `stanli_check` sha256 |
| --- | --- | --- | --- |
| A | origin/main `1ebe01c5` plus the driver-only commit `f5fdc139` (cycles in `bench_grad`) | `881d5e9f` | `e41acd87` (same bytes as the pure origin/main build; its `bench_grad` is `57131bcd`) |
| B | `a12c313d`, all switches off, layout control for A | `c0ec3ae3` | `95cacaa6` |
| C | B with `STANLI_FAST_REDUCE=1` (V1, 4 accumulators); C8 is `=2` (8 accumulators) | B | B |
| D | B with `STANLI_FAST_NOCHECK=1` (V3) | B | B |
| E | B with both | B | B |
| F | `a12c313d` built with `-DSTANLI_FD_FMA=ON` (V2), switches off | `4a922ffc` | `96e0cec9` |
| G | F with V1 and V3 on | F | F |
| Z | copy of B (A/A) | `c0ec3ae3` | |
| B2, H, D2, Y | `6e49f5af` (adds V3b): B2 all off, H `STANLI_FAST_CHECK=1`, D2 `STANLI_FAST_NOCHECK=1`, Y copy of B2 | `7967ff2c` | `54bfb5e5` |

Every timed arm runs through `/usr/bin/env` with the same set of switch variables
(`0` or `1`) and a binary path of the same length, so environment size and argv do
not differ between arms. B is 96,640 bytes larger than A in `bench_grad` (V1 and V3
code), B2 33,504 bytes larger than B. The F build has 1,514 more fused
multiply-add class instructions than B.

Timing is `harnesses/ab_bench_corpus.py` (`bench_grad --timed`, 200 ms warmup, 250 ms
window, fresh process per sample, one thread, one process at a time, first arm
rotating each round, arms interleaved) with a new per-sample record of cycles and
instructions for the measured window from `proc_pid_rusage`. Models: the 319 corpus
models whose lowered graph has an op of normal, cauchy, student_t, lognormal, beta
or gamma (`harnesses/fd_census.py`, vectorizing-stanc MIR as the sweep times it). 197
have an argument longer than one, 122 are scalar-only; 317 time (`s2_invgaussian` and
`sir` fail at the benchmark point in A). Eight rounds per model, 24 for the nine
starter models in `/Users/xitrium/claud/stanli-sulong/models`. One-minute load
average before each model: median 3.28, maximum 4.56, one 30 second pause at 5.15
(a load limit of 5). The 95% intervals resample models and, within a model, rounds.

## Measured outcome

### Speedup over B (same layout), 317 models

Each cell is the geometric mean over models, 95% interval in parentheses, then the
same figure from cycles per gradient.

| arm | all, 317 | vector-argument, 196 | scalar-only, 121 |
| --- | --- | --- | --- |
| C, V1 | 1.0060 (1.0031 to 1.0097), cycles 1.0075 | 1.0095 (1.0058 to 1.0144), cycles 1.0120 | 1.0005 (0.9959 to 1.0055), cycles 1.0003 |
| F, V2 | 1.0000 (0.9965 to 1.0047), cycles 1.0013 | 0.9998 (0.9955 to 1.0059), cycles 1.0008 | 1.0003 (0.9946 to 1.0076), cycles 1.0021 |
| D, V3 | 1.0297 (1.0227 to 1.0373), cycles 1.0315 | 1.0469 (1.0367 to 1.0574), cycles 1.0492 | 1.0024 (0.9966 to 1.0088), cycles 1.0033 |
| E, V1+V3 | 1.0393 (1.0308 to 1.0482), cycles 1.0401 | 1.0628 (1.0507 to 1.0751), cycles 1.0641 | 1.0024 (0.9961 to 1.0084), cycles 1.0024 |
| G, V1+V2+V3 | 1.0433 (1.0348 to 1.0536), cycles 1.0448 | 1.0697 (1.0563 to 1.0842), cycles 1.0705 | 1.0021 (0.9967 to 1.0097), cycles 1.0044 |
| Z, A/A | 0.9982 (0.9946 to 1.0018), cycles 1.0000 | 0.9987 (0.9939 to 1.0030), cycles 0.9998 | 0.9973 (0.9918 to 1.0039), cycles 1.0003 |
| B over A, layout | 1.0039 (1.0008 to 1.0076), cycles 1.0036 | 1.0023 (0.9980 to 1.0067), cycles 1.0022 | 1.0064 (1.0012 to 1.0124), cycles 1.0060 |

Over A the arms read C 1.011, D 1.035, E 1.044, F 1.005, G 1.048 (all models).

V3 grows with the longest argument. Geometric mean over models, nanoseconds then
cycles:

| longest fused argument | models | C (V1) | D (V3) | E | F (V2) | G | Z (A/A) |
| --- | ---: | --- | --- | --- | --- | --- | --- |
| 1 | 121 | 1.001 / 1.000 | 1.002 / 1.003 | 1.002 / 1.002 | 1.000 / 1.002 | 1.002 / 1.004 | 0.997 / 1.000 |
| 2 to 15 | 59 | 1.004 / 1.009 | 1.006 / 1.006 | 1.020 / 1.016 | 1.005 / 1.001 | 1.017 / 1.018 | 0.999 / 1.000 |
| 16 to 63 | 68 | 1.011 / 1.014 | 1.039 / 1.039 | 1.054 / 1.057 | 1.000 / 1.002 | 1.064 / 1.061 | 0.999 / 0.999 |
| 64 to 255 | 19 | 1.016 / 1.017 | 1.088 / 1.092 | 1.112 / 1.108 | 1.005 / 1.007 | 1.130 / 1.129 | 1.011 / 1.000 |
| 256 and up | 50 | 1.011 / 1.012 | 1.093 / 1.100 | 1.109 / 1.117 | 0.991 / 0.998 | 1.120 / 1.127 | 0.993 / 1.000 |

Best and worst models per arm, by cycles (nanoseconds in parentheses; the worst
nanosecond readings of every arm are noise, see the next section):

| arm | best | worst |
| --- | --- | --- |
| C | `eight_schools_centered` 1.101 (1.106), `ch09_m9_2` 1.097, `ch09_m9_3` 1.095 | `low_dim_gauss_mix_collapse` 0.968 (0.990), `hmm_example` 0.988 |
| D | `radon_partially_pooled_centered` 1.291 (1.302), `radon_county` 1.289 (1.304), `radon_partially_pooled_noncentered` 1.282 | `gp_regr` 0.993, `s2_gr_by` 0.993; none below 0.98 |
| E | `radon_partially_pooled_centered` 1.362 (1.369), `radon_county` 1.356, `radon_partially_pooled_noncentered` 1.344 | `gp_regr` 0.981, `one_comp_mm_elim_abs` 0.983 |
| F | `ch09_m9_3` 1.086 (1.082), `ch09_m9_2` 1.042, `ch09_m9_5` 1.037 | `ch12_m12_4` 0.938 (0.960, not confirmed, below), `dugongs_model` 0.972 |
| G | `ch09_m9_5` 1.397 (1.424), `radon_partially_pooled_centered` 1.390, `radon_county` 1.384 | `ch11_m11_6` 0.977, `s2_mm_weights` 0.982 |

Cycles per gradient confirm the direction of every arm-wide figure, and are much
tighter than nanoseconds: models more than 3% below B in nanoseconds are 14 (C),
20 (D), 23 (E), 47 (F), 19 (G) and 41 (Z, the identical copy), and in cycles 1, 0, 0,
1, 0 and 1. Models more than 5% above B by cycles: 7 (C), 64 (D), 80 (E), 1 (F), 83
(G), 1 (Z).

### Layout noise and the models that matter

B against A is 1.004 over all models, but single models move: 0.88 to 1.14 in
nanoseconds, 0.91 to 1.09 in cycles (28 models below 0.97 in cycles). The extra code
in `density_fused.cpp` changes the link layout and nothing else. Confirmation at 24
pairs, arms A, B, C, F and Z (`scratch-fd/results/confirm`):

| model | A over B | C over B | F over B | Z over B |
| --- | --- | --- | --- | --- |
| `ch15_m15_4` | 0.912 ns (MAD 0.028), 0.915 cycles (0.005) | 1.010 / 1.000 | 1.015 / 0.998 | 1.008 / 1.002 |
| `ch12_m12_4` | 1.024 / 1.005 | 0.997 / 1.001 | 0.981 / 0.999 | 1.021 / 1.007 |
| `low_dim_gauss_mix_collapse` | 1.015 / 1.002 | 0.976 / 1.002 | 0.986 / 1.004 | 0.994 / 1.001 |

The only reading below 0.97 in cycles that was not layout (`ch12_m12_4` for F at
0.938, `low_dim_gauss_mix_collapse` for C at 0.968) is 0.999 and 1.002 at 24 pairs.
`ch15_m15_4` is a layout effect: A is 8.5% slower than B, and C, F and Z agree with B.
It repeats the `bones_model` finding of the 2026-10-06 note: an A/A run reads 1.00
and cannot see it, so each arm is compared with B and A is shown separately.

Nine starter models, 24 pairs, speedup over B as nanoseconds / cycles (B ns and
cycles per gradient in the first columns):

| model | B ns | B cycles | A | C (V1) | D (V3) | E | F (V2) | G | Z |
| --- | ---: | ---: | --- | --- | --- | --- | --- | --- | --- |
| arK | 1548 | 5863 | 0.999 / 1.003 | 1.010 / 1.009 | 1.090 / 1.087 | 1.104 / 1.098 | 1.014 / 0.997 | 1.102 / 1.102 | 1.008 / 0.999 |
| arma11 | 4832 | 18561 | 0.942 / 0.938 | 0.994 / 1.004 | 1.007 / 1.012 | 1.020 / 1.017 | 1.002 / 1.004 | 1.041 / 1.031 | 1.009 / 1.002 |
| eight_schools_centered | 112 | 427 | 1.002 / 1.018 | 1.112 / 1.095 | 1.053 / 1.022 | 1.159 / 1.159 | 1.014 / 1.004 | 1.127 / 1.138 | 0.997 / 1.001 |
| eight_schools_noncentered | 110 | 424 | 1.066 / 1.039 | 1.027 / 1.035 | 1.020 / 1.042 | 1.068 / 1.079 | 1.018 / 1.010 | 1.105 / 1.083 | 1.023 / 0.999 |
| garch11 | 7130 | 27562 | 0.945 / 0.957 | 1.009 / 1.001 | 0.993 / 1.017 | 1.011 / 1.021 | 0.998 / 1.006 | 0.999 / 1.026 | 0.983 / 1.002 |
| kidscore_momiq | 1278 | 4924 | 0.993 / 0.990 | 1.009 / 1.023 | 1.225 / 1.246 | 1.257 / 1.282 | 0.957 / 0.990 | 1.299 / 1.303 | 0.999 / 0.998 |
| logearn_height | 3475 | 13220 | 0.994 / 0.986 | 1.021 / 1.022 | 1.235 / 1.231 | 1.289 / 1.265 | 1.000 / 0.987 | 1.289 / 1.284 | 0.986 / 1.000 |
| radon_pooled | 37643 | 142789 | 0.993 / 0.991 | 1.019 / 1.024 | 1.231 / 1.208 | 1.269 / 1.244 | 1.024 / 0.990 | 1.256 / 1.257 | 0.999 / 1.000 |
| radon_variable_intercept_noncentered | 44553 | 168959 | 1.002 / 0.994 | 1.025 / 1.020 | 1.186 / 1.173 | 1.230 / 1.206 | 1.014 / 0.992 | 1.238 / 1.218 | 1.002 / 1.000 |

Geometric mean over the nine, cycles: C 1.026, D 1.111, E 1.148, F 0.998, G 1.156,
A 0.990 (`arma11` and `garch11` are 5% to 6% slower in A than B, a layout effect
larger than any V1 or V2 effect on them).

### V1 accumulators

On the 69 models with an argument of 64 or more (8 rounds, arms B, C 4 accumulators,
C8 8 accumulators, Z): C 1.0115 (1.0066 to 1.0194), cycles 1.0137; C8 1.0145 (1.0055
to 1.0215), cycles 1.0131; C8 over C 0.998 (cycles 0.999). Four and eight are the
same. In Eigen's own sum the exact path keeps 2 packets (4 doubles on NEON) with a
scalar head peeled to alignment and expressions without packet support (`lgamma`
through `unaryExpr`) summed left to right. The variant keeps 4 packets (8 doubles) or
8 (16), loads unaligned from index 0, and sums unaligned expressions with 4 or 8
scalar accumulators. It therefore also changes which elements go through Eigen's
packet `log` and which through scalar `log`, so not every V1 difference is
reduction order. Only cauchy's aligned buffer was dropped (when a scalar output sums
it); the other kernels keep their materialized intermediates, which also avoid
recomputing transcendentals.

### V3b, exact checks behind one vectorized pass

V3 cannot ship as written, but the checks cost real time, so a variant keeps every
rejection and only skips the scans when one vectorized pass clears the arguments
(`(x - x).sum() == 0` for finite, `minCoeff` and `maxCoeff` for sign and range; the
existing predicates and Stan Math's checks still run, and still throw, when the pass
does not clear). 137 models with a fused-density argument of 16 or more, 8 rounds,
over B2:

| arm | all 137 | 16 to 63 (68) | 64 to 255 (19) | 256 and up (50) |
| --- | --- | --- | --- | --- |
| H, `STANLI_FAST_CHECK=1` | 1.0346 (1.0273 to 1.0428), cycles 1.0358 | 1.025 / 1.026 | 1.055 / 1.062 | 1.040 / 1.039 |
| D2, `STANLI_FAST_NOCHECK=1` (ceiling) | 1.0706 (1.0566 to 1.0845), cycles 1.0704 | 1.042 / 1.041 | 1.090 / 1.095 | 1.103 / 1.102 |
| Y, A/A | 1.0018 (0.9970 to 1.0059), cycles 1.0001 | 1.002 / 1.000 | 0.999 / 1.001 | 1.003 / 1.000 |

Starters, 24 pairs, ns / cycles over B2: `kidscore_momiq` H 1.134 / 1.127 against a
ceiling of 1.260 / 1.251, `logearn_height` 1.089 / 1.086 against 1.242 / 1.231,
`radon_pooled` 1.045 / 1.051 against 1.184 / 1.203, `radon_variable_intercept_noncentered`
1.040 / 1.044 against 1.170 / 1.173, `arK` 1.049 / 1.058 against 1.078 / 1.087; the
geometric mean over the nine is 1.048 (cycles) against 1.115. H gets about half of
the ceiling because its own pass is bound by floating-point add latency (two packet
accumulators, about one element per cycle per array). Eight accumulators, or testing
data arguments once at bind time, would shorten it; neither was tried. Output is
byte-identical to A at all 1,056 corpus points and 352 of 352 models pass
`tools/verify_refs.py`, including the points CmdStan rejects.

## Numerics

Replay of the corpus at all recorded points (352 models, 1,056 points) through
`stanli_check --wa-values`, stdout hashed and compared with A
(`harnesses/fd_replay.py`, `scratch-fd/results/replay.jsonl`), and the gate verdict
from `tools/verify_refs.py` itself through a wrapper per arm
(`scratch-fd/results/verify/`). Scaled error is `|a-b| / max(|a|, |b|, 1)` over the
log density and gradient. Raw ULP distance over all values is dominated by
components that are zero to rounding (up to 1e18 for A against CmdStan), so ULP is
reported over the 124 ULP-gated brms fixtures only.

| arm | points with a different byte | points with a different density or gradient (models) | max scaled error vs A | brms fixtures: points differing, max ULP vs A and vs CmdStan | `verify_refs.py` |
| --- | ---: | --- | --- | --- | --- |
| A, B | 0 | 0 | 0 | 0, 0 and 8 | 352 of 352 |
| C, V1 4 | 269 | 269 (121) | 1.18e-13 (`nn_rbm1bJ100`) | 69, 12 and 12 | 351 (`s2_s_by`, 12 ULP, limit 10) |
| C8, V1 8 | 279 | 279 (129) | 1.26e-13 | 74, 12 and 12 | 351 (`s2_s_by`) |
| D, V3 | 2 | 0 | 0 | 0, 0 and 8 | 351 (`s2_nlf` point 2 not rejected) |
| E, V1+V3 | 271 | 269 (121) | 1.18e-13 | 69, 12 and 12 | 350 |
| F, V2 | 149 | 140 (97), 9 more only in write_array | 1.40e-15 | 40, 12 and 12 | 351 (`s2_s_by`) |
| G, V1+V2+V3 4 | 315 | 304 (134) | 1.18e-13 | 85, 12 and 12 | 350 |
| G8 | 318 | 307 (139) | 1.26e-13 | 84, 24 and 24 | 350 |
| H (V3b) | 0 | 0 | 0 | 0, 0 and 8 | 352 of 352 |

The worst scaled error against CmdStan over every arm is `gpcm_latent_reg_irt` at
9.38e-13, the same as A, so no arm moves the corpus maximum. V3 changes no value at a
valid point (1,054 of 1,056 outputs byte-identical); at `s2_nlf` points 1 and 2,
which A and CmdStan reject, D returns lp -inf instead of throwing. Models that
reject at the benchmark point (`s2_invgaussian`, `sir`) fail in A and are not timed.
In the timing runs the density and gradient of D equal those of B on every model.

## Accuracy against a 70-digit reference

`harnesses/fd_hp_reference.py` transcribes 20 models into mpmath: ten corpus models
(`kidscore_momiq`, `kidscore_momhsiq`, `kidscore_interaction`, `logearn_height`,
`logearn_interaction`, `earn_height`, `nes`, `radon_pooled`, `ch16_m16_1`, and
`s2_s_by`, the fixture that leaves the gate) and ten synthetic Stan models written
for the purpose (`scratch-fd/models/`): student_t at N = 1000 and 300, lognormal
1000 and 300, gamma 800, beta 800, cauchy 1000, normal 100000, a normal with y near
1e4, and a normal whose gradient cancels at point 0 (the data make it zero to
rounding). The `cmdstan` column of `results/hp_compare.txt` is CmdStan's recorded
value against the truth, which checks each corpus transcription. Lognormal keeps its
-0.5 log(2 pi) constant under `~`, as CmdStan does. Density and gradient at the three
corpus points, gradient by central differences at 70 digits; error is
|value - truth| in ULP of the truth. The corpus
has no lognormal, student_t, gamma or beta call over more than 544 elements, which
is why the synthetic models exist; `logistic_regression_rhs` (student_t over 1,536
parameters) is not in the set.

Per model, the worst of three points: log density in ULP, gradient error as a
multiple of 2.2e-16 times the largest gradient component.

| model | N | A | V1, 4 | V1, 8 | V2 | V1+V2+V3, 4 |
| --- | ---: | --- | --- | --- | --- | --- |
| kidscore_momiq | 434 | 0.7 / 4.31 | 1.7 / 4.31 | 0.7 / 4.31 | 0.7 / 4.31 | 0.9 / 4.31 |
| kidscore_momhsiq | 434 | 1.6 / 2.81 | 1.9 / 2.81 | 1.9 / 2.81 | 2.1 / 2.81 | 2.9 / 2.81 |
| kidscore_interaction | 434 | 1.8 / 5.55 | 1.8 / 5.55 | 0.8 / 5.55 | 1.8 / 5.55 | 0.8 / 5.55 |
| logearn_height | 1192 | 3.5 / 8.58 | 2.2 / 8.58 | 2.8 / 8.58 | 3.5 / 8.58 | 2.2 / 8.58 |
| logearn_interaction | 1192 | 3.8 / 10.76 | 0.7 / 10.76 | 1.2 / 10.76 | 3.8 / 10.76 | 1.7 / 10.76 |
| earn_height | 1192 | 7.5 / 3.78 | 1.5 / 0.76 | 4.5 / 3.03 | 7.5 / 3.78 | 1.5 / 0.76 |
| nes | 1330 | 4.1 / 5.70 | 1.1 / 5.70 | 1.2 / 5.70 | 4.1 / 5.70 | 0.8 / 5.70 |
| radon_pooled | 12573 | 13.5 / 56.94 | 10.5 / 56.94 | 5.5 / 56.94 | 12.5 / 56.94 | 11.5 / 56.94 |
| ch16_m16_1 (lognormal) | 544 | 1.8 / 3.96 | 1.8 / 2.88 | 1.8 / 2.88 | 1.8 / 3.96 | 1.8 / 2.88 |
| s2_s_by | 40 | 1.7 / 2.03 | 2.7 / 2.03 | 2.7 / 1.51 | 1.7 / 2.13 | 1.7 / 1.51 |
| syn_student_t_1000 | 1000 | 2.5 / 3.10 | 0.5 / 3.10 | 1.0 / 3.10 | 2.5 / 3.10 | 0.7 / 3.10 |
| syn_student_t_300 | 300 | 0.7 / 1.81 | 0.7 / 1.81 | 0.7 / 1.81 | 0.8 / 1.81 | 0.7 / 1.81 |
| syn_lognormal_1000 | 1000 | 0.6 / 9.21 | 0.6 / 9.21 | 0.6 / 9.21 | 0.6 / 9.21 | 0.8 / 9.21 |
| syn_lognormal_300 | 300 | 0.7 / 1.99 | 0.7 / 1.88 | 0.7 / 1.88 | 0.7 / 2.62 | 0.3 / 1.88 |
| syn_gamma_800 | 800 | 1.9 / 3.64 | 0.5 / 4.81 | 0.5 / 3.73 | 1.9 / 3.64 | 0.5 / 4.81 |
| syn_beta_800 | 800 | 250.9 / 7.06 | 169.1 / 7.06 | 24.9 / 7.06 | 250.9 / 7.06 | 169.1 / 7.06 |
| syn_cauchy_1000 | 1000 | 12.7 / 5.63 | 3.5 / 5.63 | 0.8 / 5.63 | 12.7 / 5.63 | 3.5 / 5.63 |
| syn_normal_100000 | 100000 | 9.6 / 45.54 | 2.2 / 45.54 | 4.3 / 45.54 | 9.6 / 45.54 | 3.2 / 45.54 |
| syn_normal_offset_5000 | 5000 | 6.4 / 6.78 | 2.6 / 1.41 | 3.3 / 0.78 | 5.4 / 7.68 | 2.6 / 1.41 |
| syn_normal_cancel_2000 | 2000 | 1.3 / 3.71 | 0.7 / 4.72 | 1.3 / 4.72 | 1.2 / 3.71 | 1.7 / 4.72 |

V3 (D) is identical to A in every cell (the checks do not touch values). Over all 60
model-points:

| arm | log density ULP, median / max | gradient error, median / max (units above) | gradient error vs A, geometric mean ratio | points better / worse than 0.8x / 1.25x of A | log density error vs A, geometric mean ratio |
| --- | --- | --- | --- | --- | --- |
| A | 1.29 / 250.9 | 3.27 / 56.94 | 1 | | 1 |
| V1, 4 | 0.70 / 169.1 | 2.87 / 56.94 | 0.870 | 9 / 4 | 0.641 |
| V1, 8 | 0.63 / 24.9 | 2.96 / 56.94 | 0.860 | 10 / 3 | 0.604 |
| V2 | 1.48 / 250.9 | 3.27 / 56.94 | 0.998 | 1 / 2 | 1.031 |
| V1+V2+V3, 4 | 0.68 / 169.1 | 2.87 / 56.94 | 0.866 | 10 / 4 | 0.613 |
| V1+V2+V3, 8 | 0.71 / 26.9 | 2.96 / 56.94 | 0.855 | 10 / 3 | 0.681 |

A free order is more accurate than Eigen's packet order for the log density, because
the packet order keeps two accumulators and V1 keeps four or eight. The gradient
barely moves: the per-element partials are written by the fused kernels but summed
into parameter adjoints elsewhere (`density_bwd`), so only the partials of scalar
arguments (`put`) change. The one gradient that gets worse is `syn_gamma_800`
(3.64 to 4.81 with 4 accumulators, 3.73 with 8). The `s2_s_by` point that leaves the
10-ULP gate is a component where CmdStan and A are both 47 ULP from the truth and V1
is 35: V1 is closer to the truth than the reference it fails against, which is the
case TESTING.md accepts when the high-precision reference is recorded with the
model. V2 is a tie (one point better, two worse).

## Candidates table rows

Draft rows for the Candidates table in the numerics-versus-speed note
(candidate | measured speedup | numerics change | where):

| candidate | measured speedup | numerics change | where |
| --- | --- | --- | --- |
| Free reduction order in the fused scalar-family density sums | 1.0095x over the 196 vector-argument models that call a fused density (1.0058 to 1.0144; cycles 1.012), 1.0005x over the 121 scalar-only; 1.012x over the 69 with an argument of 64 or more; best `eight_schools_centered` 1.10x, no model below 0.97 by cycles; 4 and 8 accumulators the same | log density and scalar-argument partials differ in the last bits: 269 of 1,056 corpus points (121 models), scaled error up to 1.2e-13 (`nn_rbm1bJ100`), `s2_s_by` 12 ULP from CmdStan against the 10 ULP fixture limit; against 70 digits the log density error falls (median 1.29 to 0.70 ULP) and gradients are unchanged except `syn_gamma_800` (1.3x worse) | `STANLI_FAST_REDUCE` in `runtime/kernels/density_fused.cpp`, commit `a7aee7ef` on `scratch/fast-density-variants` |
| `-ffp-contract=fast` for `density_fused.cpp` | 1.000x over 317 models (0.9965 to 1.0047; cycles 1.001); arm64 only, no FMA instruction on baseline x86-64 | 149 corpus points differ (97 models; 9 only in write_array), scaled error up to 1.4e-15, `s2_s_by` 12 ULP; accuracy against 70 digits unchanged (error ratio 0.998) | `-DSTANLI_FD_FMA=ON`, commit `a12c313d` |
| Skip the argument predicates in the fused densities (ceiling only) | 1.047x over the 196 vector-argument models (1.037 to 1.057; cycles 1.049), 1.002x over scalar-only; 1.09x to 1.10x at 64 or more elements; `radon_county` 1.30x, `kidscore_momiq` 1.25x | none at valid points; invalid arguments are no longer rejected (`s2_nlf` points 1 and 2 return lp -inf, CmdStan rejects them) | `STANLI_FAST_NOCHECK`, commit `020f259d`; the exact version is `STANLI_FAST_CHECK`, commit `6e49f5af`, 1.035x over the 137 models with an argument of 16 or more, no numerics change |
| V1 + V3 (with and without V2) | 1.063x (1.051 to 1.075) over vector-argument models, 1.039x over all 317 for V1+V3; with V2 1.070x and 1.043x; the sum of the parts | as V1; plus V3's lost rejections | the switches above in one build |

## Wiring to a per-model `fast_math` flag

Today no per-model setting reaches a kernel. The fused kernel is chosen in
`STANLI_DEFINE_DENSITY_FWD` (`runtime/kernels/densities_impl.hpp`, line 372:
`fused_kernel_for(code)`, then `fused_density_active()`, a process-wide environment
switch) and in `ologistic_fwd` (`runtime/kernels/matrix_fns.cpp`, line 1473). The
executor binds one forward function per op when it is built
(`runtime/src/executor.cpp`, the loop near line 503 over `kernel(op.opcode)`), and
`KernelCtx` (`runtime/include/stanli/kernel_types.hpp`) has no per-model field; the
variant byte is full (bits 0 to 5 activity mask, 6 elementwise, 7 propto).
`origin/fastmath/mode` already carries `CompileOptions::fast_math` into lowering and
the passes; nothing carries it to the executor.

The same two steps serve all three variants:

1. Carry the flag to the executor: a `bool fast_math` on `Graph` or `Executor`, set
   from `compile_options.fast_math` (about 10 lines across `lower.cpp`, `compile.cpp`
   and `executor.cpp`).
2. At the executor binding loop, replace `k.forward` of the seven fused opcodes by a
   fast entry when the flag is set (about 15 lines), taken from a
   `fast_kernel_for(opcode)` next to `fused_kernel_for` in `density_fused.hpp`.

Per variant:

- V1 and V3 need no separate artifact. Template the six `*_summed` kernels and the
  ordered_logistic kernel on `bool Fast` in place of the two globals
  (`g_fast_reduce`, `g_fast_nocheck`) and add the dispatch table. The V1 and V3
  commits change 100 lines of `density_fused.cpp` (75 added, 25 removed); the
  template form is the same size. Code growth is what the env-switch build shows: +96,640 bytes
  (0.32%) in `bench_grad` for both variants in one instantiation set, the same order
  for a second instantiation set.
- V2 needs a second compile of the same source. A translation unit that includes
  `density_fused.cpp` inside another namespace and is built with
  `-ffp-contract=fast` (6 lines of CMake, the same shape as the existing per-source
  `COMPILE_OPTIONS` for the wasm build). The object is 425 KB of text (about 1.4%
  of the 30 MB executable). It is the same library, not a separate artifact, on
  arm64. On baseline x86-64 the flag does nothing, and an effect there would need
  `-mfma`, which makes it the same run-time dispatch or separate-artifact question as
  the AVX2 kernels.
- V3b (exact) needs no flag: put the vectorized pass in the default kernels
  (about 70 lines, `cheap_*` helpers and the six guard conditions). It is on the
  scratch branch behind `STANLI_FAST_CHECK` only so it could be timed against its
  own switch-off arm.

## Recommendation

| candidate | worth shipping in fast mode | geomean over affected models | vector-argument models | worst model | accuracy |
| --- | --- | --- | --- | --- | --- |
| V1 free reduction order | no on its own (free once a `Fast` template exists) | 1.006 over 317 (1.003 to 1.010) | 1.0095 (1.006 to 1.014), cycles 1.012 | 0.968 by cycles (`low_dim_gauss_mix_collapse`, 1.002 at 24 pairs) | log density more accurate, gradients level; leaves the 10-ULP gate on `s2_s_by` while closer to the truth than CmdStan |
| V2 FMA contraction | no | 1.000 over 317 (0.9965 to 1.005) | 0.9998 | 0.938 by cycles (`ch12_m12_4`, 0.999 at 24 pairs) | unchanged; leaves the gate on `s2_s_by` |
| V3 skip checks | no, unsafe | 1.030 over 317 (1.023 to 1.037) | 1.047 (1.037 to 1.057) | 0.993 by cycles | identical at valid points; loses rejections |
| V3b exact checks | yes, in the default path | 1.035 over the 137 with an argument of 16 or more (1.027 to 1.043) | same set | 0.976 (`arma11`, whose A/A reads 0.922 in nanoseconds and 0.995 in cycles) | identical output |

If one item from this note goes into fast mode it is none of the three; the gains
are in the checks, and they can be had exactly.

## Limits

- One machine (M3 Ultra, 2-wide NEON packets) and one benchmark point per model
  (point 0 of the corpus, `bench_grad` in a tight loop, not sampling). The
  `bones_model` note found a CPU-state effect that tight loops see and sampling does
  not; no arm here was checked under sampling.
- Eight rounds per model. Nanosecond ratios of single models carry 3% to 5% noise
  (the identical copy Z reads 0.94 to 1.07 on individual models); cycles per
  gradient carry about 0.5%. The geometric means are the evidence; single-model
  nanosecond figures are not.
- Layout moves single models by up to 9% in cycles (B against A) and is not
  removable; every arm is compared with B, whose code layout it shares.
- The 70-digit set is 20 models and three points each. The synthetic models are
  written for the test, not taken from the corpus, and the corpus has no long
  lognormal, student_t, gamma or beta vectors. `logistic_regression_rhs` is not in
  it. Gradient errors of the density kernels are mostly independent of V1 because
  the parameter-adjoint sums are not in these kernels.
- x86-64, wasm, Windows and the R and Python builds were not run.
- Why V1 gives 6% to 10% on `eight_schools_centered` and `ch09_m9_2` to `ch09_m9_5`
  was not investigated.
- The note index and catalog were not updated (scratch branch).

## Next question

How much of the V3 ceiling is recoverable exactly: V3b reaches half with a
two-accumulator pass; a pass with eight accumulators, and testing a data argument
(`y`, flagged inactive in `ctx.in_adj`) once per bind instead of per gradient, may
reach most of 1.07x over the 137 models without a flag.

## Evidence

Branch `scratch/fast-density-variants` in `/Users/xitrium/claud/stanrt/.worktrees/fast-density`:
`f5fdc139` cycles in `bench_grad` and the harness switches, `a7aee7ef` V1,
`020f259d` V3, `a12c313d` V2, `25010224` replay and summary scripts, `0f0c4bad`
70-digit script, `6e49f5af` V3b. Raw results are under `scratch-fd/` (untracked
where large): `results/corpus/results.jsonl` (317 models, every sample, cycles,
instructions, values, load), `results/starters/`, `results/confirm/`,
`results/v1acc/`, `results/v3b/`, `results/v3b_starters/`, `results/replay.jsonl`
and `replay2.jsonl` (per point, per arm), `results/verify/*.txt` (the gate verdicts),
`results/hp_truth.json` and `hp_compare.txt`, `results/census.json`, the summaries
`results/bench_summary.txt`, `v1acc_summary.txt`, `v3b_summary.txt`,
`replay_summary.txt`, the model lists `results/models_vector.txt` and
`models_scalar.txt`, the frozen binaries in `scratch-fd/bin/`, and the build and
sweep scripts. `results/discarded_starters_unmatched_env/` is a first starters run
discarded because only some arms carried environment variables.

### Models with a vector argument (197) and scalar-only (122)

Vector argument, from `results/models_vector.txt`: 2pl_latent_reg_irt, GLMM1_model,
GLMM_Poisson_model, Mh_model, Mtbh_model, Mth_model, aalto_grp_aov,
aalto_grp_prior_mean, aalto_grp_prior_mean_var, aalto_lin, aalto_lin_std,
aalto_lin_std_t, accel_gp, accel_splines, arK, arma11, blr, bones_model,
bym2_offset_only, ch09_m5_8s, ch09_m5_8s2, ch09_m9_1, ch09_m9_1_chains4, ch09_m9_2,
ch09_m9_3, ch09_m9_4, ch09_m9_5, ch11_m11_10, ch11_m11_11, ch11_m11_4, ch11_m11_5,
ch11_m11_6, ch11_m11_7, ch11_m11_8, ch12_m12_1, ch12_m12_2, ch12_m12_4, ch12_m12_5,
ch12_m12_6, ch12_m12_7, ch13_m13_1, ch13_m13_2, ch13_m13_3, ch13_m13_4, ch13_m13_4b,
ch13_m13_4nc, ch13_m13_5, ch13_m13_6, ch14_m14_1, ch14_m14_2, ch14_m14_3, ch14_m14_4,
ch14_m14_4x, ch14_m14_5, ch14_m14_7, ch14_m14_8nc, ch15_m15_1, ch15_m15_2, ch15_m15_5,
ch15_m15_6, ch15_m15_7, ch16_m16_1, covid19imperial_v2, covid19imperial_v3,
ctsem_ctsm, diamonds, dogs, dogs_nonhierarchical, dugongs_model, earn_height,
eight_schools_centered, eight_schools_noncentered, election88_full, garch11,
gp_pois_regr, gpcm_latent_reg_irt, grsm_latent_reg_irt, hier_2pl, hierarchical_gp,
hmm_gaussian, i320_gp_expquad, i320_gp_matern32, i320_sratio_cs, i320_sratio_plain,
iohmm_reg, irt_2pl, kidscore_interaction, kidscore_interaction_c,
kidscore_interaction_c2, kidscore_interaction_z, kidscore_mom_work, kidscore_momhs,
kidscore_momhsiq, kidscore_momiq, kilpisjarvi, log10earn_height, logearn_height,
logearn_height_male, logearn_interaction, logearn_interaction_z,
logearn_logheight_male, logistic_regression_rhs, logmesquite, logmesquite_logva,
logmesquite_logvas, logmesquite_logvash, logmesquite_logvolume, losscurve_sislob,
lotka_volterra, low_dim_gauss_mix, low_dim_gauss_mix_collapse, lsat_model, mesquite,
multi_occupancy, nes, nn_rbm1bJ10, nn_rbm1bJ100, normal_mixture, normal_mixture_k,
one_comp_mm_elim_abs, pilots, prophet, radon_county, radon_county_intercept,
radon_hierarchical_intercept_centered, radon_hierarchical_intercept_noncentered,
radon_partially_pooled_centered, radon_partially_pooled_noncentered, radon_pooled,
radon_variable_intercept_centered, radon_variable_intercept_noncentered,
radon_variable_intercept_slope_centered, radon_variable_intercept_slope_noncentered,
radon_variable_slope_centered, radon_variable_slope_noncentered, rats_model, s2_car,
s2_car_esicar, s2_car_icar, s2_cens_interval, s2_cumulative_cauchit,
s2_cumulative_cloglog, s2_cumulative_probit, s2_custom_vreal, s2_dist_sigma_re,
s2_gp_approx, s2_gp_by_approx, s2_gp_by_gr, s2_gr_by, s2_hurdle_cumulative,
s2_index_mi, s2_me2, s2_me2_nomecor, s2_mi_lognormal, s2_mmc, s2_mo_simo_prior,
s2_mv_shared_re, s2_nl_noloop, s2_nlf, s2_s_by, s2_s_cc, s2_shifted_lognormal,
s2_t2_by, seeds_centered_model, seeds_model, seeds_stanified_model, sesame_one_pred_a,
sir, soil_incubation, state_space_stochastic_level_stochastic_seasonal,
surgical_model, sw_acat, sw_acat_cs, sw_ar, sw_arma, sw_beta, sw_cens, sw_cratio,
sw_cratio_cs, sw_cumulative, sw_cumulative_cs, sw_dist_sigma, sw_gamma, sw_gp,
sw_hurdle_lognormal, sw_lognormal, sw_ma, sw_me, sw_mono, sw_nonlinear, sw_re_slope,
sw_se, sw_spline_s, sw_spline_t2, sw_sratio, sw_student, sw_weights.

Scalar-only, from `results/models_scalar.txt`: GLM_Binomial_model, Rate_1_model to
Rate_5_model, aalto_bern, aalto_binom, aalto_binom2, aalto_binomb, ch09_mp,
ch11_m11_9, ch11_m_pois, ch12_m12_3, ch12_m12_3_alt, ch13_m13_7, ch13_m13_7nc,
ch14_m14_10, ch14_m14_11, ch14_m14_6, ch14_m14_6x, ch14_m14_9, ch15_m15_3,
ch15_m15_4, ch15_m15_8, ch15_m15_9, ch16_m16_4, cm_betadiscrete, cm_betagate,
cm_bisa, cm_choco, cm_ddm, cm_exgaussian, cm_exwald, cm_gamma, cm_geg, cm_invgamma,
cm_invgaussian, cm_invweibull, cm_lba1, cm_lba2, cm_lnr, cm_lnr_bench, cm_loggamma,
cm_lognormal, cm_logstudent, cm_logweibull, cm_rdm, cm_weibull,
extra_hurdle_poisson, gp_regr, hmm_drive_0, hmm_drive_1, hmm_example, i319_gauss_re,
i319_negbin_fixed, i319_negbin_re, i319_pois_fixed, i319_pois_re, i319_pois_re2,
i320_mi_nhanes, i320_pois_trunc_both, i320_pois_trunc_ub, kronecker_gp, s2_ar_cov,
s2_beta_binomial, s2_categorical_re, s2_com_poisson, s2_cosy, s2_cox, s2_cox_cens,
s2_custom_vint, s2_dirichlet, s2_discrete_weibull, s2_fcor, s2_frechet, s2_gev,
s2_gr_student, s2_hurdle_negbin, s2_invgaussian, s2_logistic_normal, s2_mi_trunc_lb,
s2_mixture_theta, s2_mm, s2_mm_weights, s2_multinomial, s2_mv_subset, s2_rate,
s2_sar, s2_sar_error, s2_threading, s2_unstr, s2_weights_trunc, s2_wiener,
s2_zi_asymlaplace, s2_zi_beta, s2_zoi_beta, sw_asymlaplace, sw_bernoulli,
sw_binomial, sw_categorical, sw_exgaussian, sw_gaussian, sw_hurdle_gamma,
sw_hurdle_pois, sw_mi, sw_mixture, sw_mv_norescor, sw_mv_rescor, sw_negbinomial,
sw_poisson, sw_re_bern, sw_re_gauss, sw_re_negbin, sw_re_pois, sw_skewnormal,
sw_trunc, sw_vonmises, sw_weibull, sw_zi_binomial, sw_zi_negbin, sw_zi_poisson.
