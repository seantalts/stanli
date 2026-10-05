# Density-path allocations: what changed and what was tried

Base `db9c575f`, macOS 26 arm64, Apple clang, Release (`-O3`,
`-ffp-contract=off`). Timing is `bench_grad --timed` (200 ms warmup, 250 ms
or longer window, fresh process per sample, one thread requested, arms
interleaved, first arm rotating). Speedups are old time over new time.

## On this branch

| commit | what | effect |
| --- | --- | --- |
| attribution tool | `tools/alloc_attr`: malloc interposer, per-site attribution, paired A/B timing, heap and RSS probe, one model per density | shows where allocations come from |
| fused normal and cauchy | `density_fused.cpp`, `STANLI_NO_FUSED_DENSITY=1` (oracle), `-DSTANLI_FUSED_ONLY=1`, differential test | 7, 6, 5, 4, 4, 4 mallocs per gradient to 0 on six starter models; Eight Schools 1.7x to 1.85x |
| vector views | `value_of` of an array view of a mapped rvar vector returns a map instead of a copy | 1 to 2 fewer mallocs per call on every density; library and CLI 1.1 MB smaller alone, 1.03 MB smaller together with the fused kernels |
| column and matrix maps | `value_of` and `value_of_rec` of a mapped rvar column vector or matrix return maps | GLMs, multinomial and ordered GLMs, ordered_logistic cutpoint checks; library and CLI 28 KB smaller |
| docs | `how-it-works.md` and `hacking.md` allocation sentences | stale since 2184bc6e |
| census tools | `tools/op_census.cpp`, `tools/density_census.py`, `harnesses/ab_bench_corpus.py` | used for the ranking and the corpus sweep below |

Production code: fused kernels +425 lines (including the CMake edits), vector
views +15, column and matrix maps +26.

## Results at the head against `db9c575f`

### Corpus sweep

`harnesses/ab_bench_corpus.py`: every corpus model that lowers (319 attempted,
vectorizing-stanc MIR, as `corpus_bench.py` builds it), 8 interleaved pairs per
model, both `bench_grad` binaries frozen as copies, one process at a time,
`STAN_NUM_THREADS=1`. Three models (`dogs_log`, `s2_invgaussian`, `sir`) have a
non-finite density or gradient at the benchmark point in both binaries and
are not timed, leaving 316. Machine load average (1 minute) around the models:
2.3 to 4.3, median 2.8, with another session running a long Python job.

| | models | geomean speedup | 95% bootstrap interval |
| --- | ---: | ---: | ---: |
| all | 316 | 1.088 | 1.072 to 1.103 |
| can act (normal or cauchy op, or a density op with an argument longer than one) | 291 | 1.096 | 1.079 to 1.113 |
| rest | 25 | 0.998 | 0.989 to 1.009 |
| A/A, base against a copy of itself, 70 random models | 70 | 1.001 | 0.995 to 1.007 |

Models faster than 1.02: 180. Within 2%: 107. Slower than 0.98: 29. Slower
than 0.90: 1. At 8 pairs the per-model noise is wide: in the A/A run 13 of 70
models read above 1.02 and 7 below 0.98. The interval resamples models and,
within a model, pairs.

Ten largest wins: `aalto_grp_aov` 2.07x (197 ns to 95 ns),
`eight_schools_centered` 1.84x, `eight_schools_noncentered` 1.73x,
`aalto_grp_prior_mean` 1.69x, `pilots` 1.68x, `logmesquite_logvolume` 1.65x,
`ch15_m15_2` 1.62x, `aalto_lin` 1.58x, `aalto_lin_std` 1.57x, `ch09_m9_5`
1.55x.

Ten largest regressions (8 pairs; gradient time before and after):

| model | speedup | ns |
| --- | ---: | ---: |
| `bones_model` | 0.890 | 42,650 to 47,938 |
| `dogs_hierarchical` | 0.926 | 12,674 to 13,688 |
| `soil_incubation` | 0.934 | 33,025 to 35,370 |
| `sw_mv_norescor` | 0.941 | 607 to 645 |
| `garch11` | 0.946 | 7,932 to 8,382 |
| `ch14_m14_6x` | 0.948 | 85,491 to 90,214 |
| `iohmm_reg` | 0.949 | 177,231 to 186,755 |
| `kronecker_gp` | 0.955 | 196,389 to 205,639 |
| `lotka_volterra` | 0.955 | 24,207 to 25,338 |
| `ch13_m13_7nc` | 0.956 | 36.4 to 38.0 |

Re-timing the first eight with 24 pairs and the two largest with 48 (arms:
base, an A/A copy, X2 only, X2+X3, the head):

| model | A/A | X2 | X2+X3 | head |
| --- | ---: | ---: | ---: | ---: |
| `bones_model` (24 pairs) | 1.008 (0.035) | 0.893 (0.033) | 0.920 (0.040) | 0.913 (0.035) |
| `bones_model` (48 pairs, twice) | 0.992 to 1.004 | | 0.912 to 0.913 | 0.907 to 0.912 |
| `soil_incubation` (24 pairs) | 1.000 (0.033) | 0.976 (0.042) | 0.958 (0.050) | 0.899 (0.039) |
| `soil_incubation` (48 pairs, four times) | 0.990 to 1.007 | | 0.993 to 1.005 | 0.907 to 0.928 |
| `dogs_hierarchical` (24 pairs) | 0.980 (0.158) | 0.993 (0.062) | 0.966 (0.119) | 0.872 (0.117) |
| `dogs_hierarchical` (48 pairs) | 1.042 (0.149) | | 1.031 (0.121) | 0.991 (0.185) |
| `garch11` (24 pairs) | 1.005 (0.048) | 0.978 (0.044) | 0.957 (0.066) | 0.977 (0.040) |
| `garch11` (48 pairs) | 0.996 (0.038) | | 0.970 (0.054) | 0.973 (0.042) |
| `iohmm_reg`, `sw_mv_norescor`, `ch14_m14_6x`, `wells_dae_inter_model` (24 pairs) | 0.995 to 1.010 | 0.992 to 1.016 | 0.957 to 1.001 | 0.968 to 0.999 |

(Median of within-pair ratios, base over arm; MAD in parentheses.)

`dogs_hierarchical` is too noisy to read (A/A MAD 0.15). Two models are
reproducibly 7% to 10% slower:

- `bones_model`: 0.89 in the sweep, 0.91 in three later runs, while A/A and two
  no-op size controls (an unused function added to `executor.cpp` and to
  `densities_common.cpp`) read 0.98 to 1.00. The slowdown is in the X2
  commit: the X2 binary with `STANLI_NO_FUSED_DENSITY=1` reads 0.98 and the
  head with it 1.00, against 0.90 and 0.89 with the fused kernels on. The
  model has one `normal_lpdf` call (13 elements) among about 6,000 scalar ops,
  the kernel itself is faster (29 ns against 170 ns per call), the values are
  bitwise equal, and a `sample` profile shows no density kernel in either
  run, so the cause is not identified. Running the fused path costs about
  3 to 4 microseconds elsewhere in the gradient.
- `soil_incubation`: 0.92 at the head, 1.00 at X2+X3; 96% of the time is in
  `OP_ODE`, which runs 8.6% slower. Builds with only the column-vector
  overloads or only the matrix overloads of the last commit both read 0.99,
  and the two together 0.92. That points at code generation or placement
  inside the ODE kernel; no view has a cost here.

No model is reproducibly below 0.90 and no popular model (the nine starters,
`eight_schools*`, `radon*`, `kidscore*`, `logearn*`, `earn*`, `arK`, `arma11`,
`garch11`, `dogs*`, `wells*`, `seeds*`, `rats_model`, `surgical_model`,
`mesquite`, `nes*`) is: `garch11` is 0.95 in the sweep and 0.97 to 0.98 at 24
and 48 pairs, `dogs_hierarchical` 0.93 and unreadable, `wells_dae_inter_model`
0.97 and 1.00.

### Starter models and three GLM and matrix models

24 interleaved pairs, with an A/A copy, on the MIR of the earlier starter
set; base ns and speedup (MAD), then A/A:

| model | base ns | speedup | A/A |
| --- | ---: | ---: | ---: |
| eight_schools_centered | 213 | 1.835 (0.019) | 0.996 (0.010) |
| eight_schools_noncentered | 195 | 1.709 (0.009) | 1.001 (0.010) |
| arK | 1,743 | 1.140 (0.009) | 0.997 (0.010) |
| arma11 | 4,591 | 1.052 (0.025) | 1.004 (0.014) |
| garch11 | 7,337 | 1.012 (0.012) | 1.013 (0.014) |
| kidscore_momiq | 1,606 | 1.249 (0.015) | 1.007 (0.005) |
| logearn_height | 4,322 | 1.237 (0.011) | 1.007 (0.012) |
| radon_pooled | 46,692 | 1.251 (0.008) | 1.006 (0.009) |
| radon_variable_intercept_noncentered | 53,414 | 1.204 (0.015) | 1.001 (0.007) |
| brms-sw_categorical | 1,176 | 1.042 (0.012) | 0.992 (0.012) |
| brms-sw_cumulative | 22,144 | 1.008 (0.014) | 0.994 (0.007) |
| ch12_m12_5 | 2,035,197 | 1.088 (0.012) | 0.998 (0.007) |

In the corpus sweep (vectorizing MIR, 8 pairs) the same nine read 1.84, 1.73,
1.15, 1.01, 0.95, 1.18, 1.23, 1.14 and 1.19; the MIR is not the same and
`garch11` and `arma11` differ most.

`eight_schools_centered`, 48 pairs per comparison, build order rotating:
the head against base 1.852 (0.018), X2 only 1.845 (0.022), X2+X3 1.843
(0.022), A/A 0.999 (0.010). The head against X2+X3 is 0.994 (0.012) with an
A/A of 0.999 (0.011). The head is not slower than X2 here.

### Allocations, size and memory

Mallocs and bytes per gradient (two timed runs under the interposer):

| model | base | head |
| --- | ---: | ---: |
| eight_schools_centered | 7, 448 B | 0 |
| arK | 6, 6,320 B | 0 |
| garch11 | 5, 8,000 B | 0 |
| radon_pooled | 4, 402,336 B | 0 |
| logearn_height | 4, 38,144 B | 0 |
| kidscore_momiq | 4, 13,888 B | 0 |
| brms-sw_categorical | 14, 5,992 B | 11, 5,624 B |
| brms-sw_cumulative | 22, 80,208 B | 18, 70,992 B |
| ch12_m12_5 | 109,233, 1,231,464 B | 99,301, 754,728 B |

Size, bytes (gzip -6): library `libstanli.dylib` 39,960,336 (12,388,867) to
38,900,960 (11,971,537), -1,059,376 (-417,330); CLI `stanli_check` 39,989,088
(12,466,962) to 38,929,952 (12,043,488), -1,059,136 (-423,474). With
`STANLI_FUSED_ONLY=1` the library shrinks a further 584,992 (152,925 gzipped).

Peak RSS, median of 9 fresh processes per cell, base then head: within the
roughly 100 KiB run-to-run spread on eleven models; `radon_pooled` is 21,712
to 22,032 KiB (+1.5%), the `sw_categorical` and `sw_cumulative` models
80 to 160 KiB lower.

### Correctness

- Differential test (both paths in one process): 43,462 cases, 8,463
  rejections with equal messages, 0 ULP for both densities and every mask;
  also in the lite build.
- Corpus replay, `tools/verify_refs.py` through a wrapper that runs two
  `stanli_check` binaries and compares stdout bytes, 329 models, 987 points:
  head against base, 985 identical and 2 different: `ch14_m14_8` point 0 (log
  density, 2 ULP) and `ch15_m15_8` point 1 (log density, 1 ULP). Head against
  itself with `STANLI_NO_FUSED_DENSITY=1`: 987 identical. 329 of 329 inside the
  corpus gate, worst `gpcm_latent_reg_irt` at 9.38e-13.
- CTest 266 of 267 pass; `lit_passes_ark_reroll_fma` fails, and fails on base.
- The two differing points are the zero-copy views reading an executor slot
  that starts at an odd double (below).

## Where the allocations came from

`tools/alloc_attr` records a backtrace per malloc and diffs two iteration
counts. On eight_schools_centered, arK, garch11, radon_pooled, logearn_height
and kidscore_momiq every allocation per gradient was an Eigen temporary inside
one `normal_lpdf` call (7, 6, 5, 4, 4, 4); recorder edges, the executor and
nested tapes allocate nothing. After the branch what remains is Stan Math
temporaries in other densities (ordered_logistic builds about ten arrays per
call, 99,301 mallocs per gradient in `ch12_m12_5`), the nested-`var` copies of
the legacy kernels (`matrix_fns.cpp` `mvt_vectors`, the `normal_id_glm` var
vectors) and one `std::vector<int>` per ordered_logistic call.

### value_of inventory

| site | operand | class |
| --- | --- | --- |
| `recorder.hpp` generic `value_of(const T&)` | any rvar expression | lazy unary expression; kept for non-map shapes |
| ArrayWrapper overloads | array view of a mapped rvar vector | zero-copy |
| column and matrix map overloads | `Map<const Matrix<rvar,-1,1>>`, `Map<const Matrix<rvar,-1,-1>>`, `value_of` and `value_of_rec` | zero-copy |
| `densities_glm.cpp`, `matrix_fns.cpp` GLMs and ordered_logistic | parameter vectors, design matrices, cutpoints | zero-copy through the map overloads |
| `densities_lpmf.cpp` | double map; integer outcomes | no rvar operand; int to double is needed |
| `quadrature.cpp`, `matrix_fns.cpp` `inverse_spd`, `model_adapter.hpp` | var results, the sampler's var vector | needed |
| `mir_interp.hpp`, `mir_prog.hpp`, `program.hpp`, `program.cpp` | scalar register reads and a register gather | needed; registers hold `var` or `double` |
| `dirichlet_lpdf` | `vector_seq_view::val` into its own arrays | Stan Math's copy |

No kernel builds a row vector, a `std::vector<rvar>`, a strided map or a
non-const rvar map, so none needs an overload.

## Tried and not kept

### Per-thread arena for Eigen temporaries (X1)

Redirected `malloc`, `realloc` and `free` inside Eigen's headers in every
translation unit to a bump arena reset per density kernel. It took the six
starters to 0 mallocs per gradient and sped Eight Schools up 1.25x and the
others 1.0x to 1.07x, but it lost to the fused kernels and views on every
model, added 221 KB to the library, needed every translation unit to agree
on the redirect, and made the Eigen-heavy controls 1% to 5% slower (`gpcov`
0.95x). Reusing Stan Math's arena instead was a change of the same size and
slower. Kept on the `spike/kernel-alloc` branch.

### Starting vector slots on a 16 byte boundary

Hypothesis: the two log densities the views move come from executor slots at
an odd double (Eigen peels leading elements of a misaligned map before it
vectorizes a sum). A trace shows one view call in each of the two models, at
8 bytes off a 16 byte boundary, and copying that one buffer aligned restores
the base bytes. On 186 rethinking and brms models 35.7% of the 112,773 kernel
inputs of length two or more started off a 16 byte boundary (35,936 written
slots, 2,435 data, 1,846 parameters).

Rounding written and data slots of length two or more up to an even double
(8 production lines, a layout test) made the views bitwise: head with
alignment against base with alignment, 987 of 987 points identical. It was
not adopted:

- it changes four other corpus points relative to base (`iohmm_reg`
  `write_array` at three points by 1 ULP, moving toward CmdStan; one
  `one_comp_mm_elim_abs` gradient component by 32 ULP, 1344 to 1312 ULP from
  CmdStan), none in a log density;
- it speeds up nothing on arm64 (within 2% on 20 models);
- the arenas grow: 0 to 88 bytes on the starters, 1.5% across the corpus,
  16% (23 KB) at worst in `brms-sw_acat_cs`;
- parameters cannot be padded, so 1,846 inputs (1.6%) stay misaligned;
- `eight_schools_centered` timed 1.5% to 2% slower on an identical layout,
  which no-op controls showed is within code-placement noise but is the
  size the issue-374 bar rejected.

Aligning adjoint slots and scratch bases as well changed no model
consistently. The measurement tool (`#define private public` over
`Executor`) and the change are on the `spike/density-fused` branch. The test
in the vector views commit pins the dependence: bitwise for an aligned buffer,
at most 2 ULP one double in.

## Which densities to fuse next

Frequency from `tools/density_census.py` (319 corpus models lowered; bound
ops, ops with an argument longer than one, and the sum of the longest argument
per gradient); cost per call from `tools/alloc_attr/density_models.py` (64
observations, parameter-derived vector location, parameter scalars) against a
base model of 1 malloc and 152.5 ns; size is the text bytes of the
`stan::math::<density><...rvar...>` instantiations in the runtime kernel
objects (lower bounds).

| density | models | ops (vector) | elements | mallocs per call | ns per 64 | instantiations, text bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| normal (fused) | 199 | 2,491 (1,972) | 300,242 | 0 | 175 | 160, 227,720 |
| cauchy (fused) | 17 | 27 (1) | 28 | 0 | 378 | 134, 236,640 |
| ordered_logistic | 7 | 29,824 (29,824) | 178,836 | 12 | 2,673 | 8, 21,584 |
| bernoulli_logit | 18 | 35 (34) | 51,201 | 1 | 464 | 16, 13,016 |
| binomial | 27 | 33 (18) | 8,949 | 2 | 1,155 | 16, 29,828 |
| poisson_log | 11 | 19 (12) | 4,299 | 1 | 301 | 6, 9,760 |
| bernoulli | 25 | 63 (21) | 4,133 | 1 | 466 | handwritten |
| std_normal | 32 | 51 (51) | 2,567 | 0 | 76 | 2, 2,280 |
| student_t | 133 | 275 (28) | 1,905 | 3 | 496 | 580, 1,556,300 |
| poisson | 21 | 507 (15) | 1,568 | 1 | 302 | 6, 10,080 |
| lognormal | 17 | 30 (17) | 866 | 2 | 373 | 150, 348,452 |
| binomial_logit | 10 | 201 (10) | 792 | 2 | 1,156 | 24, 32,264 |
| beta | 31 | 110 (1) | 149 | 2 | 449 | 236, 304,728 |
| gamma | 26 | 28 (4) | 116 | 0 | 355 | 130, 282,288 |
| exponential | 44 | 70 (13) | 115 | 0 | 47 | 20, 27,944 |

The GLMs (`bernoulli_logit_glm`, 12 models and 165,783 elements;
`normal_id_glm`, 23 models and 127,246) and `multi_normal` (14 models,
70,861) are larger by volume but are a matrix-vector product around the
density, or run on the nested `var` tape, so a fused kernel is a different
project.

Estimated fused lines (no code written): lognormal 45; gamma and beta 55
each; student_t 110 (a four-argument path); exponential 70; ordered_logistic
90 (integer outcomes, shared cutpoints); the discrete densities 60 each.

1. `ordered_logistic`: the only remaining density with a large per-call cost
   (12 mallocs, 2.7 microseconds at N=64) and 99,301 of the mallocs per
   gradient in `ch12_m12_5`. Seven corpus models, 90 lines, 21 KB of text.
2. `student_t` if size is the aim: 1.56 MB of instantiations, the largest of
   any density; speed matters little (28 of its 275 ops are vectors).
3. `lognormal`, `beta`, `gamma` together, about 300 KB each.
4. Leave the discrete densities and `exponential`: 0 to 2 mallocs, 10 to 32 KB.
   `bernoulli_logit`'s one malloc is an `ArrayXd` temporary in stanli's own
   kernel (`densities_lpmf.cpp`), removable in place without a new kernel.

## Not tested

x86-64, wasm, Windows, R and Python builds, and compilers other than Apple
clang with libc++; the `STANLI_FUSED_ONLY` build's own test suite (only its
size was measured); the ASan build. The Mac was shared: during the sweep the
one-minute load average was 2.3 to 4.3 (median 2.8) with another session's
long job running.

## Fusing ordered_logistic and student_t

Two more kernels in `density_fused.cpp`, one commit each, following the
normal and cauchy pattern (run-time mask and propto, Stan Math's own checks
on rejection, `STANLI_NO_FUSED_DENSITY=1` as the oracle, `STANLI_FUSED_ONLY`
removes the Stan Math path). Both differential tests require 0 ULP and
get it; each was seen to fail when the kernel was wrong (a flipped cutpoint
partial: 9.6e18 ULP; materializing the y and mu derivative of student_t
unconditionally: 2e8 ULP, so the test is sensitive to the `to_ref_if` rules
that fix Eigen's summation order).

| | ordered_logistic (shared cutpoints) | student_t |
| --- | ---: | ---: |
| differential cases, rejections, max ULP | 7,566, 1,878, 0 | 61,741, 14,145, 0 |
| production lines added | 146 (+24 in `matrix_fns.cpp`) | 205 |
| mallocs per call at N=64, before to after | 13 to 0 | 3 to 0 |
| library, both paths kept | +20,128 B | +109,776 B |
| library, `STANLI_FUSED_ONLY` | -15,616 B | -1,791,744 B (-553,324 gz) |
| instantiations removed (text, lower bound) | 8 (21,584 B) | 580 (1,556,300 B) |

With both together `STANLI_FUSED_ONLY` is 1,807,360 bytes smaller than the
previous head (38,315,968 to 36,508,608; CLI 38,344,944 to 36,537,680;
gzipped library 11,818,925 to 11,266,405). Both paths kept, the library is
129,904 bytes larger (+51,326 gzipped).

Corpus replay (987 points) against the previous head: byte-identical, and
byte-identical to the same build with the oracle switch. CTest 268 of 269
(`lit_passes_ark_reroll_fma`, which fails on base). The lite build passes
the four fused tests. Peak RSS (median of 9) is within noise on ten of twelve
models; `garch11` is 128 KiB higher (8,032 against 7,904), `sw_student` 240
and `s2_hurdle_cumulative` 176 KiB lower.

### Speed

Paired gradient timings, `ab_bench_corpus.py`, 8 pairs per model, binaries
frozen, one thread, three arms in the same rounds: base (`db9c575f`), the
previous head and this head. 316 of 319 models time (the three that are
non-finite at the benchmark point on base are excluded). One-minute load
average 2.3 to 5.8 (median 2.9): `spotlightknowledged` held it at 3.5 to
4.2 for 25 minutes, and the harness was restarted with a load limit of 5
after waiting that long.

| comparison | models | geomean | 95% bootstrap interval |
| --- | ---: | ---: | ---: |
| previous head to this head, all | 316 | 1.010 | 1.002 to 1.020 |
| models that use ordered_logistic or student_t | 136 | 1.025 | 1.009 to 1.046 |
| all other models | 180 | 0.999 | 0.995 to 1.005 |
| A/A (previous head against a copy), 60 random models | 60 | 1.004 | 0.996 to 1.010 |
| base to this head, all | 316 | 1.096 | 1.079 to 1.114 |
| base to previous head, earlier sweep | 316 | 1.088 | 1.072 to 1.103 |

Faster than 1.02: 64 models. Within 2%: 201. Slower than 0.98: 51. Slower
than 0.90: none. Against base: 189 faster than 1.02, 102 within 2%, 25 slower
than 0.98, none below 0.90.

ordered_logistic, previous head to this head (16 pairs, A/A 0.97 to 1.01):
`ch12_m12_5` 1.92x, `ch12_m12_7` 1.93x, `ch12_m12_6` 1.44x,
`s2_hurdle_cumulative` 2.12x, `ch12_m12_4` 1.015x. (Mallocs per gradient:
`ch12_m12_5` 99,301 to 1, `s2_hurdle_cumulative` 272 to 2.)

student_t: geometric mean 1.025 over the 26 models with a vector call
(`aalto_lin_std_t` 1.25x, `sw_student` 1.20x, `sw_spline_t2` 1.07x,
`s2_gp_by_approx` 1.06x, `s2_t2_by` 1.05x; the lowest are
`i320_sratio_cs` 0.96x and `state_space_stochastic_level_stochastic_seasonal`
0.96x), and 1.002 over the 105 models whose calls are all scalar (range 0.97 to
1.06). The scalar calls had nothing to remove.

The ten largest regressions in the all-model comparison are 0.94 to 0.96 (the
44 ns `ch13_m13_7`, `normal_mixture_k`, `arma11`, `hmm_drive_0`,
`radon_variable_slope_centered`, `state_space_stochastic_level_stochastic_seasonal`,
`ch14_m14_1`, `logmesquite_logvolume`, `radon_county`, `i320_sratio_cs`);
none use either density except the last two, and re-timing the first six
with 24 pairs gave 0.98 to 1.00 against A/A of 0.99 to 1.02. The nine starter
models at 24 pairs: 0.991 to 1.012 for this head against the previous one,
A/A 0.995 to 1.005.

### What it does not cover

An array of cutpoint vectors still goes through the nested-tape path. A
scalar location with more than one outcome is undefined in Stan Math; the
fused kernel raises a size mismatch there. Not tested: x86-64, wasm, Windows,
R and Python builds.

## Shipping it: release configuration and rebase onto 3cecda71

### Artifact map

Every released artifact is built from `CMakeLists.txt`; none has a second build
system for the runtime. The option `STANLI_STAN_DENSITY_ORACLE` (default ON)
is set OFF where an artifact ships:

| artifact | built by | OFF set in |
| --- | --- | --- |
| Linux and macOS wheels, runtime tarballs, the CLI, the R package's runtime | `wheels.yml` job `build`, `build-rel`, then `tools/build_wheel.sh` | the `Configure and build` step |
| Windows wheel (MSYS2 clang) | `wheels.yml` job `Configure, build, test` | its configure line |
| browser module and webR side module | `wheels.yml` (`build-wasm`, `build-wasm-side`), locally `tools/build_web.sh` | both emcmake lines, `build_web.sh` |
| conformance runtime (nightly) | `stan-conformance-nightly.yml`, target `stanli_shared` | its configure line |
| compiler cache seed | `clang-ccache-seed.yml` | its configure line (the flag changes the compile commands, so the seed must match) |
| local `build-rel` | `tools/dev_setup.sh` | `dev_setup.sh` (its `build` keeps the option ON) |

`tools/build_wheel.sh` builds `stanli_shared` from a configured `build-rel`
and now warns when that tree has the option ON. The lite build
(`STANLI_LITE_LP`) is a manual build and keeps the default. The sanitizer and
development-setup jobs build with the default (ON) and so run the
differential tests; PR CI builds that configuration once more, in the Linux
x86-64 wheel job (`Differential tests against Stan Math`). The R and Python
installed-artifact checks and the recorded corpus replay run on `build-rel`
and the wheels, that is, on the OFF configuration. No required status was
renamed or removed.

### The option

`STANLI_STAN_DENSITY_ORACLE` ON keeps Stan Math's kernels for the fused
densities beside the fused ones (for `STANLI_NO_FUSED_DENSITY=1` and the three
differential tests); OFF compiles them out by defining `STANLI_FUSED_ONLY`
for the runtime. It defaults ON so a plain `cmake -B build` builds and runs
the whole test suite without extra flags; release paths pass OFF explicitly.
Removed under OFF: the `rvar` instantiations of `normal_lpdf`, `cauchy_lpdf`
and `student_t_lpdf` in the summed and elementwise density kernels (every
shape and activity mask is covered by a fused kernel) and of
`ordered_logistic_lpmf` for a shared cutpoint vector. Kept, because no fused
kernel covers them: the array-of-cutpoints path, the nested-`var` kernels,
and the `var` and `double` instantiations used by Program regions. With OFF,
`STANLI_NO_FUSED_DENSITY=1` prints a notice at load and is ignored.

### Rebased head against origin/main 3cecda71

Conflicts: only `CMakeLists.txt` (the test list, four times, as upstream added
tests to the same lines) and the stale sentence in `docs/how-it-works.md`
(upstream rewrote the file; the sentence moved and was reworded). #429 changed
how `density_bwd` contracts partials and added a micro-op path that reads
`scratch + offset` and the connected flag; the fused kernels write the same
layout, so no change was needed, and the differential tests, which compare the
scratch contents, pass.

- Warnings: all three builds (origin/main, ON, OFF) from empty build trees give
  3 linker notices (the embedded stanc object was built for macOS 26 and is
  linked for 11.0), identical on origin/main, and no compiler warning.
- Differential tests, ON build: 43,462 / 61,741 / 7,566 cases, 0 ULP.
- CTest: ON 398 of 398, OFF 395 of 395.
- Corpus replay (352 models, 1,056 points), byte-compared: head (OFF) against
  origin/main differs at one point, `ch14_m14_8` point 0, log density by 2 ULP
  (`ch15_m15_8` point 1, which differed on the old base, is identical now);
  head (OFF) against the ON build with `STANLI_NO_FUSED_DENSITY=1`: 1,056 of
  1,056 identical. 352 of 352 inside the corpus gate.
- Size, bytes (gzip -6), against origin/main: library 42,616,752 (12,846,665)
  to 39,118,368 (11,701,378), -3,498,384 (-1,145,287); CLI `stanli_run`
  42,635,528 (12,928,784) to 39,137,176 (11,773,557), -3,498,352
  (-1,155,227). The ON build is 976,208 smaller than origin/main (library).
- Starter models, 8 pairs only (not the 24 asked for), one thread, speedup over
  origin/main (A/A 0.988 to 1.010): ON build 1.82, 1.72, 1.14, 1.05, 1.01,
  1.24, 1.24, 1.31, 1.25; OFF build 1.94, 1.84, 1.14, 1.04, 1.02, 1.24, 1.25,
  1.30, 1.26 for eight_schools_centered, eight_schools_noncentered, arK,
  arma11, garch11, kidscore_momiq, logearn_height, radon_pooled,
  radon_variable_intercept_noncentered. Removing the dead code did not slow
  any of them; Eight Schools is faster, as it was for the earlier fused-only
  build (no out-of-line oracle-switch check).

### Not done on the new base

The corpus geomean sweep (the 1.096x was measured against db9c575f), the
24-pair starter timings, the peak RSS comparison, and the wheel, R package,
browser build and installed-artifact checks were not run here: the 22:30 limit
for the machine came first, and the wheel, R and wasm toolchains were not
exercised. `actionlint` is not installed; the workflow YAML parses and
`tests/test_ci_policy.py` passes.
