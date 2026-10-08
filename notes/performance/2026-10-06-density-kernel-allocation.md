# Density-path allocations: what changed and what was tried

macOS 26 arm64, Apple clang, Release (`-O3`, `-ffp-contract=off`). Timing is
`bench_grad --timed` (200 ms warmup, 250 ms or longer window, fresh process
per sample, one thread requested, arms interleaved, first arm rotating).
Speedups are old time over new time.

Several bases appear. `db9c575f` is the old base, the commit the first
measurements compared against. `3cecda71` is origin/main before PR #439 and
`80c3ea47` is that PR's merge; the section "Corpus sweep, starters and memory
against 3cecda71" is the measurement of the merged work against its real
base. Figures in the sections before it are against the old base. `29856a71`
is origin/main after `80c3ea47`; the head sizes and the sweep in the section
"Fusing lognormal, beta and gamma" are against it.

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

## Results against the old base `db9c575f`

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
- it speeds up nothing on arm64 (within 2%, not consistently, on the nine
  starters and seven vector-heavy corpus models at 12 pairs);
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

Update: `student_t` and `ordered_logistic` with a shared cutpoint vector were
fused next (section "Fusing ordered_logistic and student_t"), and `lognormal`,
`beta` and `gamma` after that (section "Fusing lognormal, beta and gamma",
which also records the array form of `ordered_logistic`, tried and dropped).

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

An array of cutpoint vectors goes through the nested-tape path; fusing it was
tried and dropped (section "Fusing lognormal, beta and gamma"). A scalar
location with more than one outcome is undefined in Stan Math; the fused kernel raises a size
mismatch there. Not tested: x86-64, wasm, Windows,
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

### Corpus sweep, starters and memory against 3cecda71

`80c3ea47` against `3cecda71`, both Release builds with
`STANLI_STAN_DENSITY_ORACLE=OFF`. `harnesses/ab_bench_corpus.py` over every
corpus model that lowers (342 attempted), 8 interleaved pairs per model,
binaries frozen as copies, one process at a time, one thread, the first arm
rotating each round. Three models (`dogs_log`, `s2_invgaussian`, `sir`) fail
at the benchmark point in the base binary and are not timed, leaving 339.
One-minute load average around the models: 2.5 to 4.3, median 3.2, with
another session running.

| | models | geomean speedup | 95% bootstrap interval |
| --- | ---: | ---: | ---: |
| all | 339 | 1.098 | 1.081 to 1.115 |
| can act (a normal or cauchy op, or a density op with an argument longer than one) | 310 | 1.108 | 1.090 to 1.128 |
| rest | 29 | 0.997 | 0.988 to 1.008 |
| A/A, base against a copy of itself, 60 random models | 60 | 0.998 | |

Faster than 1.02: 201 models. Within 2%: 117. Slower than 0.98: 21. Slower
than 0.90: none. One model's values differ between the arms, `ch14_m14_8` (the
2 ULP log density below).

Ten largest wins: `s2_hurdle_cumulative` 2.23x (5,445 ns to 2,447 ns),
`ch12_m12_5` 2.15x, `ch12_m12_7` 2.11x, `aalto_grp_aov` 2.10x,
`eight_schools_centered` 1.93x, `eight_schools_noncentered` 1.82x,
`aalto_grp_prior_mean` 1.78x, `pilots` 1.74x, `aalto_lin_std` 1.66x,
`ch09_m9_5` 1.65x.

Ten largest regressions: `bones_model` 0.920 (47,861 ns to 52,041 ns),
`sw_acat` 0.922, `aalto_poisson_hurdle` 0.943, `Rate_5_model` 0.951,
`ch14_m14_6` 0.952, `i320_pois_trunc_ub` 0.956, `gp_regr` 0.962, `irt_2pl`
0.966, `sw_gp` 0.969, `i319_gauss_re` 0.972.

At 24 pairs with an A/A arm: `bones_model` 0.932 (MAD 0.033, A/A 1.006), so it
reproduces and its cause is still unknown. It is the model with one fused
`normal_lpdf` call among about 6,000 scalar ops that the old-base sweep also
flagged. `sw_acat` is 0.989 (A/A 0.994) and
`aalto_poisson_hurdle` 1.007 (A/A 1.014), so those two were noise.

Nine starter models, 24 interleaved pairs, base ns and speedup (MAD), A/A:

| model | base ns | speedup | A/A |
| --- | ---: | ---: | ---: |
| eight_schools_centered | 213.0 | 1.931 (0.021) | 0.995 (0.009) |
| eight_schools_noncentered | 195.6 | 1.844 (0.029) | 1.003 (0.009) |
| radon_pooled | 48,916 | 1.303 (0.008) | 1.003 (0.007) |
| radon_variable_intercept_noncentered | 55,388 | 1.250 (0.011) | 0.999 (0.014) |
| kidscore_momiq | 1,603.7 | 1.242 (0.009) | 1.001 (0.005) |
| logearn_height | 4,314.5 | 1.238 (0.009) | 0.995 (0.008) |
| arK | 1,755.8 | 1.146 (0.008) | 0.999 (0.007) |
| arma11 | 5,079.5 | 1.045 (0.008) | 1.003 (0.018) |
| garch11 | 7,344.8 | 1.026 (0.008) | 1.006 (0.008) |

Peak RSS of the same nine, median of 9 fresh processes, KiB:

| model | base | new |
| --- | ---: | ---: |
| eight_schools_centered | 5,360 | 5,264 |
| eight_schools_noncentered | 5,520 | 5,472 |
| arK | 8,528 | 8,512 |
| arma11 | 7,936 | 7,936 |
| garch11 | 8,256 | 8,224 |
| kidscore_momiq | 5,600 | 5,616 |
| logearn_height | 5,712 | 5,760 |
| radon_pooled | 22,400 | 22,304 |
| radon_variable_intercept_noncentered | 37,760 | 37,680 |

The largest increase is 48 KiB (`logearn_height`) and the largest decrease 96
KiB (`eight_schools_centered` and `radon_pooled`).

The raw files of this sweep are not in the repository.

### Not done on the new base

The wheel, R package, browser build and installed-artifact checks were not run
by this note's author; the PR ran the wheels workflow on the branch by hand
and it passed. `actionlint` is not installed; the workflow YAML parses and
`tests/test_ci_policy.py` passes.

## Fusing lognormal, beta and gamma

Three more kernels in `density_fused.cpp`, one commit each, after PR #439. Same
pattern: run-time mask and propto, Stan Math's own checks on rejection,
intermediates materialized where Stan's `to_ref_if` does, the
`STANLI_NO_FUSED_DENSITY=1` oracle, and the Stan Math instantiations compiled
out under `STANLI_STAN_DENSITY_ORACLE=OFF`. The per-density measurements
below were made against `80c3ea47`, one stage at a time. The head sizes and
the final sweep are against origin/main `29856a71`, which adds an AVX2 option
that is off by default, notes and CI tooling to `80c3ea47` and has an OFF
library of the same size.

Before fusing each density, the allocations per gradient attributed to it were
counted on every corpus model whose source mentions it (`tools/alloc_attr`
with a script that labels each allocation site by the Stan Math density in its
backtrace, 1,000 against 11,000 gradients).

| | lognormal | beta | gamma |
| --- | ---: | ---: | ---: |
| corpus models whose source mentions it | 24 | 34 | 42 |
| of those, models with 0.5 or more allocations per gradient from it | 12 | 1 | 3 |
| allocations per gradient at sites inside Stan Math's density, before, over those models | 46.4 | 4.2 | 3.6 |
| production lines added (net) | 118 | 87 | 61 |
| library with the option OFF, bytes (gzip -6) | -366,528 (-107,229) | -352,592 (-89,754) | -317,712 (-105,019) |
| `rvar` instantiations of Stan Math removed, text bytes | 150, 348,452 | 236, 304,728 | 130, 282,288 |
| speedup on the models that use it, 16 pairs (95% interval) | 1.036 (1.012 to 1.063) | 1.029 (1.016 to 1.042) | 1.006 (0.998 to 1.016) |
| A/A on the same models | 1.001 | 0.997 | 0.998 |

At 64 observations the #439 census measured 2 allocations per call for
lognormal, 2 for beta and 0 for gamma; nothing was measured per call after. The
new kernels keep their work arrays on the stack up to 128 elements and use one
`std::vector` above that, so a call allocates nothing up to 128 elements and
once beyond. The whole-model totals from the corpus fell with them:
`aalto_grp_prior_mean_var` 7.8 to 0, `s2_shifted_lognormal` 11.9 to 0,
`losscurve_sislob` 6.3 to 0, `sw_beta` 9.3 to 1.0 and `sw_gamma` 4.2 to 1.0
per gradient, the remainder coming from other ops.

The first density's 118 lines include the helpers the other two reuse. The
speedups compare a build with the density fused against the build one commit
earlier, arms in the same rounds (`base`, the three stages, and a copy of
`base`), over 90 models in all. The models with allocations: lognormal 12
models 1.054 (1.016 to 1.102), the best `aalto_grp_prior_mean_var` 1.21x,
`sw_lognormal` 1.17x, `s2_shifted_lognormal` 1.14x; beta's one model,
`sw_beta`, 1.07x; gamma's three models 1.037 (0.973 to 1.085), `sw_gamma` 1.09x
and `covid19imperial_v2` 0.970. The other beta models gain too, with
`s2_zoi_beta` 1.11x, `aalto_binom2` 1.10x and `Rate_4_model` 1.09x: their
calls are scalar, and the fused scalar path is cheaper than the Stan Math one.
Gamma has no measurable speed effect and 0 to 1.6 allocations per gradient in
three of 42 models; its case is the size, 318 KB.

Library sizes for the OFF configuration, bytes (gzip -6):

| build | library | `stanli_run` |
| --- | ---: | ---: |
| origin/main `29856a71` | 39,118,368 (11,701,356) | 39,137,176 (11,773,711) |
| lognormal | 38,751,840 (11,594,053) | 38,770,728 (11,670,821) |
| beta | 38,399,248 (11,504,299) | 38,418,024 (11,579,826) |
| gamma, head | 38,081,536 (11,399,394) | 38,116,824 (11,476,961) |

The head is 1,036,832 bytes smaller (301,962 gzipped) than origin/main with the
option OFF, and 3,703,728 smaller than the head built with it ON (41,785,264;
gzipped 12,503,201 against 11,399,394). `stanli_run` is 1,020,352 smaller
(296,750 gzipped). The lognormal and beta rows were measured at their own
commits on the earlier base. After the head, no `rvar` instantiation of
`ordered_logistic_lpmf` and none of `lognormal_lpdf`, `beta_lpdf` or
`gamma_lpdf` on a vector argument remains in the OFF build. What is left of
the three densities is 42, 34 and 34 symbols: seven scalar `rvar` combinations
of each, and the `var` and `double` code for Program regions in
`program_density.cpp` and two instantiations in `matrix_fns.cpp`. The 17
`ordered_logistic_lpmf` symbols that remain are `var` code for the array of
cutpoint vectors, the same 17 as in origin/main.

### Tried and dropped: the array form of ordered_logistic

A commit fused `ordered_logistic_lpmf` for an array of cutpoint vectors by
giving the shared-cutpoint kernel a per-observation cutpoint offset. It made
the library 57,632 bytes smaller (12,660 gzipped) with the option OFF, and a
synthetic model with 64 outcomes and `rep_array(c, N)` as the cutpoints went
from 12.6 to 2.7 microseconds per gradient. No corpus model passes an array of
cutpoint vectors. It was dropped by decision and is not part of this change.
The array form still runs through Stan Math, in release builds too, and
`test_glm` checks it against the Stan Math `var` path bitwise.

### Micro models

`tools/alloc_attr/density_models.py` models (64 observations, a vector location
that depends on parameters, parameter scalars), 16 pairs against the base:
lognormal 522 ns to 452 ns (1.14x), beta 591 ns to 535 ns (1.08x), gamma 492 ns
to 476 ns (1.05x), the same model with no density under test 144 ns to 137 ns
(1.05x; it has a scalar gamma prior). A/A on each is 0.99 to 1.00. The
per-call allocation counts of the attribution tool were too noisy to use on
these models (the model without a density read 1.0 to 12.1 allocations per
gradient across repeats), so no per-call figure after the change was measured.
The corpus totals above are the evidence that the Stan Math sites are gone and
that whole-model allocations fell.

### Correctness

- Differential tests (both paths in one process, ON build): `test_density_fused`
  135,535 cases over normal, cauchy, lognormal, beta and gamma with 27,783
  rejections, `test_ordered_logistic_fused` 7,566 cases (shared cutpoints) with
  1,878 rejections, `test_student_t_fused` 61,741 cases with 14,145; every
  rejection has the same message; 0 ULP in all of them.
- Each test was seen to fail when a kernel was wrong: an extra factor on the
  lognormal sum (2.1 million ULP), a sequential instead of Eigen's reduction
  order for `sum(log_y)` (up to 24 ULP), and the wrong size divisor in beta
  (1.5e19 ULP) and gamma (1.5e19 ULP).
- Corpus replay (352 models, 1,056 points), outputs byte-compared: head with
  the option OFF against origin/main with it OFF, 1,053 identical and 3
  different, all three `ctsem_ctsm` points with `--wa-values`, where only the
  build id in a stderr warning differs (`abi1-` followed by the checked-out
  commit) and stdout is identical; head OFF against the head built with the
  option ON and `STANLI_NO_FUSED_DENSITY=1`, 1,056 identical. 352 of 352
  inside the corpus gate, worst `gpcm_latent_reg_irt` at 9.38e-13.
- An array of cutpoint vectors, which is not fused, in a model
  (`vector[N] lambda; array[N] ordered[K - 1] c; y ~ ordered_logistic(lambda,
  c)`): the output at three points is byte-identical for the OFF head, the ON
  head, the ON head with `STANLI_NO_FUSED_DENSITY=1` and origin/main OFF.
  `test_glm` runs the array form through the executor in both configurations
  and compares value and gradients bitwise with Stan Math's `var` path.
- Builds from empty trees, option ON and OFF: no compiler warning. Three linker
  notices about the embedded stanc object, as on origin/main, and 44 stanc
  notes about test fixtures, as on origin/main.
- CTest: ON 398 of 398, OFF 395 of 395.

### Corpus sweep against 29856a71

The head against origin/main `29856a71`, both Release builds with
`STANLI_STAN_DENSITY_ORACLE=OFF` from empty trees, binaries frozen as copies.
`harnesses/ab_bench_corpus.py` over every corpus model that lowers (342
attempted), 8 interleaved pairs per model, one thread, one process at a time,
the first arm rotating, `--load-limit 5` (run from a copy that polls the load
every 3 seconds instead of every 30). Three models (`dogs_log`,
`s2_invgaussian`, `sir`) fail at the benchmark point in the base binary and are
not timed, leaving 339. The one-minute load average around the models was 2.6
to 5.1, median 3.3, with a one-core Python job from another project running.

| | models | geomean speedup | 95% interval |
| --- | ---: | ---: | ---: |
| all | 339 | 1.005 | 1.001 to 1.009 |
| source calls lognormal, beta or gamma | 71 | 1.031 | 1.019 to 1.043 |
| the rest | 268 | 0.998 | 0.995 to 1.002 |
| A/A, base against a copy, 70 random models | 70 | 1.005 | 0.999 to 1.011 |

Faster than 1.02: 75 models. Within 2%: 214. Slower than 0.98: 50. Slower than
0.90: one, `sw_skewnormal`. In the A/A run 16 of 70 models read above 1.02 and 6
below 0.98. The geometric mean over all models is inside the A/A interval, so
the sweep supports the 71 models that call these densities and nothing more.
No density or gradient value differs between the arms.
Largest wins: `aalto_grp_prior_mean_var` 1.22x, `sw_lognormal` 1.20x,
`losscurve_sislob` 1.13x, `sw_hurdle_lognormal` 1.13x, `s2_shifted_lognormal`
1.13x, `s2_zoi_beta` 1.11x, `s2_mi_lognormal` 1.09x, `aalto_binom2` 1.09x,
`Rate_4_model` 1.09x, `sw_beta` 1.09x.
Largest regressions: `sw_skewnormal` 0.802, `surgical_model` 0.941,
`rats_model` 0.944, `s2_s_cc` 0.945, `sw_mono` 0.949.

Re-timing these and the other popular models below 0.97, 24 pairs with an A/A
arm (speedup, A/A): `surgical_model` 1.003 (1.000), `rats_model` 0.988 (1.007),
`s2_s_cc` 0.992 (1.008), `sw_mono` 1.009 (1.017), `ch15_m15_9` 0.990 (0.993),
`low_dim_gauss_mix_collapse` 0.990 (1.004), `kidscore_momhs` 0.977 (0.989),
`s2_gp_by_approx` 0.998 (0.993), `dogs_hierarchical` 1.007 (1.013),
`seeds_model` 1.006 (0.999) and `logearn_interaction` 1.029 (1.018). Only
`sw_skewnormal` stays slow: 0.803 at 24 pairs against an A/A of 0.986. No
popular model is more than 10% slower and one model is.

`sw_skewnormal` calls `student_t`, `normal`, `skew_normal` and `student_t_lccdf`
and none of the three fused densities. The same head built with the option ON
reads 0.806, and 0.802 with `STANLI_NO_FUSED_DENSITY=1`. An earlier build of
this series that also fused the array form of `ordered_logistic` reads 0.954
against `29856a71`, and the build of `80c3ea47` reads 0.968. Appending 12 or 36
`nop` instructions (48 or 144 bytes) to `density_fused.cpp` gives the head 1.019
and 1.031, and appending 1 or 4 does not (0.810 and 0.826). So the slowdown is
a code-placement effect of the same kind as `bones_model` below and not a cost
of the change. The function whose placement matters was not identified.

The nine starters at 24 pairs, speedup over `29856a71` (MAD), A/A (MAD):

| model | speedup | A/A |
| --- | ---: | ---: |
| arK | 0.957 (0.029) | 0.973 (0.022) |
| arma11 | 1.012 (0.050) | 0.980 (0.062) |
| eight_schools_centered | 1.006 (0.037) | 1.014 (0.041) |
| eight_schools_noncentered | 0.997 (0.034) | 1.016 (0.041) |
| garch11 | 0.973 (0.023) | 1.013 (0.044) |
| kidscore_momiq | 0.989 (0.038) | 0.989 (0.032) |
| logearn_height | 0.995 (0.020) | 0.995 (0.029) |
| radon_pooled | 0.999 (0.036) | 0.998 (0.038) |
| radon_variable_intercept_noncentered | 1.019 (0.020) | 1.004 (0.020) |

`arK` and `garch11` read 2% to 4% slower; their A/A arms read 0.973 and 1.013,
and neither uses the three densities.

Peak RSS of the nine starters, median of 9 fresh processes, KiB:

| model | `29856a71` | head |
| --- | ---: | ---: |
| eight_schools_centered | 4,864 | 4,832 |
| eight_schools_noncentered | 5,104 | 5,088 |
| arK | 8,448 | 8,464 |
| arma11 | 7,760 | 7,648 |
| garch11 | 7,968 | 7,872 |
| kidscore_momiq | 5,296 | 5,232 |
| logearn_height | 5,552 | 5,504 |
| radon_pooled | 9,504 | 9,472 |
| radon_variable_intercept_noncentered | 42,608 | 42,832 |

The largest increase is 224 KiB (`radon_variable_intercept_noncentered`, 0.5%);
the run-to-run spread is up to 500 KiB on that model and about 100 KiB on the
others.

### Not tested

x86-64, wasm, Windows, R and Python builds, the ASan and TSan builds, the lite
build, and the wheels workflow. The Mac was shared; the load average during
the sweeps was 2.6 to 5.1.

## bones_model: a CPU-state effect in back-to-back gradients

Update to the earlier "cause unknown" entries. `bones_model` reads 0.91 to 0.93
(old over new) in `bench_grad --timed` against base, at 24 pairs: 0.913 for the
option-ON build, 0.923 for the release build, 1.002 for the ON build with
`STANLI_NO_FUSED_DENSITY=1`, and 0.994 for A/A. The model has 7,527 ops, 9
distinct opcodes and 6,841 opcode changes per gradient.

What was measured, on the same frozen binaries:

- The P core alternates between about 4.05 and 3.62 GHz every 0.1 to 0.5 s in
  both arms. That is the timing noise and not the arm gap. At equal clock the
  IPC is 2.22 to 2.26 with the fused kernels and 2.42 without; instructions
  retired are equal.
- Thread QoS classes (user-interactive, user-initiated, default, utility), busy
  threads on other cores and the DIT bit change nothing. Background QoS moves
  the thread to an E core.
- Random placement of allocations of 2 KiB or more moves the arm ratio between
  0.92 and 1.02. Uniform shifts do nothing, some staggered shifts remove the
  effect, and malloc salting does nothing.
- A no-op call every 500 ops, 8,192 extra branches, 96 KB of straight-line code,
  a 1 MiB sweep, barriers and atomics do not restore the speed. Any kernel entry
  or any malloc and free of 16 B to 64 KiB between gradients does, and the
  effect of a `getpid` call fades within about 17 microseconds.
- Page faults are 0 and reclaims about 1,210 in both arms.

In seeded NUTS sampling the cycles per gradient, old over new, are 1.001 for the
release build and 1.007 for the ON build (12 pairs), with IPC 2.44 to 2.46 in
every arm. The slow state needs gradients back to back with no allocation or
system call between them, so sampling does not reach it. The hardware structure
involved is not identified, because there are no performance counters here.

Consequences for measurement, not adopted anywhere yet:

- Report cycles per gradient from `proc_pid_rusage` next to nanoseconds. The
  median absolute deviation falls from 0.031 to 0.007.
- Confirm any `bench_grad` ratio below 0.97 with seeded sampling cycles per
  gradient before calling it a regression.
- An A/A run cannot detect this effect. It reads 1.00 while allocation layout
  moves the arm gap between 0.92 and 1.02.
- A system call per gradient is not a fix: about 0.3 microseconds would swamp
  small models.

The scripts are on the local scratch branch `density-fused-bones2`
(`scratch-bones2/`), not on main.

## sw_skewnormal: a page-boundary effect in the benchmark binary

After #442 `sw_skewnormal` reads 0.80x against 29856a71 in `bench_grad`
(0.802 at 8 pairs and 0.803 at 24 pairs in this sweep, 0.827 in the 2026-10-07
corpus run). It calls only `normal_lpdf` and `student_t_lpdf`, which both builds
already fuse. A fresh `origin/main` build reads 0.80; the same source with a
driver-only change to `bench_grad.cpp` reads 0.95. `sample` puts the difference
in `run_program_impl<false,double>`: 1,126 samples in the slow build and 728 in
the fast one.

The function's hot dispatch head and vector-multiply loop span its first 0x204
bytes, so a 4 KB page boundary falls inside that region when the function starts
at page offset 0xe00 or 0xf00. Starting offsets 0, 0x400, 0x500, 0x800, 0xc00,
0xc40 and 0xf40 are fast; this was tested with `aligned(N)` variants in both
layouts. The shipped library, `stanli_run` and the #444 head sit in fast
placements, so users do not see the slowdown; the `bench_grad` driver, which
links the runtime statically, does in some builds. `aligned(4096)` on the
function removes the sensitivity and is neutral on 27 other models plus the
starters (cycles 1.005), but grows the library by 1.54 MB (4%), so nothing was
shipped. Evidence: `scratch-fd/results/sk` in the local worktree used for the
fast-density measurements (branch `scratch/fast-density-variants`).
