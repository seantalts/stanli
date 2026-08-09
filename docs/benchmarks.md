# Benchmarks vs CmdStan

2026-08-06, Apple M-series (macOS arm64), clang, both sides -O3 and
-ffp-contract=off, single-threaded. Both engines evaluate the sampling
gradient (propto + jacobian) at the same deterministic unconstrained
point. stanli runs `tools/bench_grad.cpp`; CmdStan runs
`tools/bench_cmdstan_grad.cpp`, looping the same fresh-vars + grad +
recover_memory cycle `stan::model::gradient` performs per leapfrog
step. Reproduce the whole table with
`tools/bench_models.py deps/cmdstan deps/posteriordb`.

## Per-gradient latency

A representative slice; the complete 120-model table is at the bottom of
this page.

| model | unconstrained params | stanli ns/grad | CmdStan ns/grad | speedup | stanli prep |
| --- | ---: | ---: | ---: | ---: | ---: |
| `radon_pooled` | 3 | 52,902 | 320,938 | 6.07x | 0.083 s |
| `arK` | 7 | 2,382 | 12,459 | 5.23x | 0.007 s |
| `radon_hierarchical_intercept_centered` | 391 | 111,649 | 569,143 | 5.10x | 0.179 s |
| `radon_county_intercept` | 388 | 89,669 | 431,614 | 4.81x | 0.132 s |
| `nes` | 10 | 19,696 | 69,324 | 3.52x | 0.032 s |
| `eight_schools_noncentered` | 10 | 227 | 745 | 3.28x | 0.006 s |
| `election88_full` | 90 | 295,265 | 901,961 | 3.05x | 0.379 s |
| `bym2_offset_only` | 3845 | 39,582 | 114,620 | 2.90x | 0.046 s |
| `dogs` | 3 | 22,014 | 63,747 | 2.90x | 0.042 s |
| `kidscore_momiq` | 3 | 1,889 | 4,861 | 2.57x | 0.008 s |
| `lsat_model` | 1006 | 45,543 | 91,173 | 2.00x | 0.055 s |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 17,196 | 26,320 | 1.53x | 0.023 s |
| `normal_mixture` | 3 | 78,960 | 88,239 | 1.12x | 0.091 s |
| `low_dim_gauss_mix` | 5 | 88,949 | 98,315 | 1.11x | 0.099 s |
| `wells_dist100ars_model` | 3 | 17,431 | 18,997 | 1.09x | 0.025 s |
| `radon_county` | 389 | 83,236 | 82,076 | 0.99x | 0.105 s |
| `arma11` | 4 | 6,650 | 6,158 | 0.93x | 0.012 s |
| `diamonds` | 26 | 35,358 | 31,497 | 0.89x | 0.110 s |
| `garch11` | 4 | 11,184 | 9,664 | 0.86x | 0.016 s |
| `hmm_drive_0` | 6 | 172,957 | 132,850 | 0.77x | 0.189 s |
| `hmm_example` | 4 | 36,275 | 27,145 | 0.75x | 0.046 s |
| `ldaK2` | 7 | 145,916 | 104,059 | 0.71x | 0.165 s |
| `iohmm_reg` | 29 | 545,184 | 320,335 | 0.59x | 0.441 s |

## Which models are faster, which are slower, and why

Across the full corpus (`docs/corpus-bench.tsv`, 119 models with both
gradients) the median per-gradient speedup is 2.07x and 93 of 119
models are at or above parity. The ratio is predicted almost entirely
by the model's *shape*, not its size.

**Faster (most of the corpus, typically 1.5-6x):**

- **Vectorized-statement models.** A `y ~ normal(X * beta, sigma)` over
  N observations is a handful of ops here; CmdStan builds and walks N
  var-tape nodes per statement per leapfrog step. The gap grows with N.
  This class is regressions, GLMs, and most hierarchical models.
- **Scalar loops the passes can vectorize.** The hierarchical indexing
  idiom (`y[n] ~ normal(mu[county[n]], sigma)` and loops that fill a
  vector element by element) arrives unrolled and is re-rolled back
  into the class above: the radon family up to 6.1x, `election88_full`
  3.0x, `dogs` 2.8x.
- **Everything, on preparation.** Lowering takes 4-400 ms against a
  ~7 s CmdStan compile, so short runs and iterative model development
  are dominated by this regardless of gradient speed.

**Near parity (0.8-1.2x):** models dominated by one large dense
operation (a Cholesky, a big matrix product, the GP models). Both
engines spend their time inside the same stan-math kernel; interpreter
overhead is noise on top.

**Slower (a shrinking tail, mostly 0.5-0.8x):**

- **Sequential models.** HMM recursions and state-space/ARMA/GARCH
  updates read the previous step's parameter-dependent result, so
  re-rolling correctly refuses (vectorizing a recurrence would change
  the math). What is left is per-op dispatch against CmdStan's inlined
  scalar code; a native adjoint program is what would change that.
- **Mixture models with K > 2 components** (`ldaK2`/`ldaK5`): their
  inner `log_sum_exp` runs over K-vectors built per document, a
  row-wise reduction the elementwise-lp fusion does not yet express.
  The binary-mixture case is closed.
- **ODE models**, at about 0.6x: our per-call dispatch of the compiled
  right-hand side against CmdStan's fully inlined one inside the same
  CVODES solver. (They were 0.015x before the right-hand side compiled;
  see below.)
- **`Mtbh_model` (0.45x)**, the slowest model, spends 36% in
  `OP_SET_SLICE_STRIDED`. The in-place rule rewrites element writes but
  not slice writes, so a model that fills a matrix column by column
  still copies the whole matrix per column. Open.

A profile of every sub-parity model (`STANLI_PROFILE=1`) says the
remaining tail is mostly not a graph problem: in seven of those models
a single precompiled kernel is half to nine-tenths of the gradient.
`diamonds` was the extreme, 90.9% in one GLM kernel that rebuilt a var
tape in both sweeps; differentiating once and stashing the partials
took it 0.48x -> 0.89x. `prophet` was 82% in `OP_MATVEC` with one
serial accumulation chain; four independent accumulators took it 0.67x
-> 1.23x, bitwise unchanged.

## Where the wins come from

The interpreter's cost is per op, not per element: ~17-20 ns for a
scalar density op forward + backward, against ~0.9 ns for the actual
math (`tools/bench_opcost.cpp`). A vectorized statement over N elements
amortizes that to nothing and runs precompiled stan-math on contiguous
doubles, while CmdStan pays its var-tape cost per scalar per
evaluation. Vectorized models therefore win, and models stuck as
unrolled scalar loops used to lose (0.4-0.9x) until the graph passes
closed the gap. The passes themselves are described in
[runtime/src/OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md); the
headline measurements:

- **Re-rolling** (scalar loops back to vector ops): `radon_pooled`
  27,670 ops -> 8 (0.91x -> 6.18x), `arK` 3,164 -> 21 (0.40x -> 4.83x).
- **In-place updates + store-to-load forwarding** (the element-write
  idiom): `radon_county_intercept` went from 90.5 ms per gradient in
  2.58 GB of arena (207x slower than CmdStan) to 92 us in 42 MB, 77,960
  ops -> 9. Seven radon-family models and `rats_model` collapse the
  same way.
- **Write-side fusion** (loops that fill a vector something else
  reads): `radon_county` 25,152 ops -> 10 (0.36x -> 0.98x),
  `election88_full` 289,165 -> 65 (0.39x -> 2.97x). Strided-run and
  integer-outcome fusion closed the `dogs` family (0.65x -> 2.8x).
  57 of the 120 corpus models change under the passes, against 28
  before write-side fusion.
- **Elementwise-lp fusion** (the mixture idiom): `low_dim_gauss_mix`
  7,208 ops -> 16, crossing parity (0.78x -> 1.07x); `normal_mixture`
  13 ops, 1.09x.

## Tape islands, measured

The island pass compiles irreducible scalar residue (recurrences) into
one register-machine op. The op collapse is dramatic on every model
that has such a region; the time follows on exactly one. Measured with
`STANLI_NO_ISLAND=1` as the baseline, same build, same point, on all
fourteen corpus models that compile a region:

| model | islanded | ops off | ns/grad off -> on | |
| --- | ---: | ---: | ---: | ---: |
| `iohmm_reg` | 27 ops | 53,456 | 1,432,673 -> 547,895 | **2.61x** |
| `hier_2pl` | | 256 | 305,182 -> 306,487 | 1.00x |
| `hmm_gaussian` | 11 ops | 42,926 | 368,160 -> 365,965 | 1.01x |
| `multi_occupancy` | | 353 | 69,105 -> 70,981 | 0.97x |
| `hmm_example` | 13 ops | 3,483 | 33,253 -> 34,385 | 0.97x |
| `hmm_drive_0` | 24 ops | 19,540 | 167,291 -> 177,449 | 0.94x |
| `hmm_drive_1` | 25 ops | 19,540 | 177,097 -> 195,421 | 0.91x |
| `accel_gp` | | 399 | 9,764 -> 10,954 | 0.89x |
| `accel_splines` | | 399 | 10,739 -> 12,208 | 0.88x |
| `Mb_model` | | 5,390 | 71,209 -> 94,210 | 0.76x |
| `losscurve_sislob` | | 230 | 2,379 -> 3,133 | 0.76x |
| `garch11` | | 1,790 | 10,931 -> 14,575 | 0.75x |
| `arma11` | | 1,198 | 6,882 -> 10,570 | 0.65x |
| `bones_model` | 77 islands | 4,955 | 52,774 -> 1,005,148 | 0.05x |

The op count was never the cost: a dispatch is ~5 ns and the scalar
kernels around it are cheap, while a var replay of the same operations
costs what CmdStan costs for them. `iohmm_reg` wins because its steps
copy a 1,500-element state vector each (1.6M elements of traffic per
gradient) and the island's registers make the copies disappear;
`bones_model` is the same mechanism inverted, a 4,024-register file
rebuilt as vars 77 times per gradient. The pass now estimates both
sides before committing; the estimate separates the fourteen exactly,
and the thirteen it leaves alone are bitwise identical to the
passes-off baseline. What would move the rest of this class is a native
adjoint program, generated alongside the forward one instead of
replayed under nested autodiff.

## ODE models

Every user function is inlined at lowering time except an ODE
right-hand side, which the integrator calls at times of its choosing.
It used to be evaluated by a tree-walking interpreter: 5.8 us per call,
~500 calls per gradient, 97% of the model's gradient time, and the
system was solved twice per gradient (values, then derivatives). Both
are gone: the right-hand side compiles once into a flat register
machine, and the forward sweep keeps the sensitivities it already
computes, so the backward is a matrix-vector product.

| model | before | after | speedup | vs CmdStan |
| --- | ---: | ---: | ---: | ---: |
| `lotka_volterra` | 2,790,941 ns | 71,704 ns | 38.9x | 0.015x -> 0.58x |
| `soil_incubation` | 3,389,538 ns | 96,362 ns | 35.2x | 0.018x -> 0.63x |
| `one_comp_mm_elim_abs` | 18,873,857 ns | 653,181 ns | 28.9x | 0.025x -> 0.74x |

Gradients are unchanged to the bit where they were before, and
`lotka_volterra` moved from 4 ULP to bitwise identical to CmdStan:
reading the jacobian out of the same solve that produced the values
removes a second, independently stepped solve. Anything the compiler
cannot express keeps the interpreter, so coverage never shrinks;
`STANLI_DEBUG_ODE=1` reports when that happens.

Preparation scales too: the largest corpus model (`nn_rbm1bJ100`,
MNIST, 60,000 rows, 79,411 parameters) lowers to a 192,030-op graph in
20.7 s and evaluates its gradient in 0.43 s. Lowering used to be
quadratic in data size (the transformed-data interpreter copied an
indexed expression's whole base per read). Overall, stanli lowers a
model in 4-200 ms against a 6.2-7.6 s CmdStan compile (warm precompiled
header, after a multi-minute one-time `make build`); that gap is what
time-to-first-draw is made of.

## End to end: eight schools, 1000 warmup + 1000 draws

| engine | stage | time |
| --- | --- | --- |
| stanli | stanc + graph compile + bind | 0.014 s |
| stanli | NUTS 1000+1000 (incl. constraining draws) | 0.020 s |
| stanli | CLI total (`stanli_run`, process start to CSV) | 0.24 s |
| CmdStan | model build (stanc + clang, warm PCH) | 4.62 s |
| CmdStan | NUTS 1000+1000 (self-reported total) | 0.044 s |
| CmdStan | build + run wall time | ~4.98 s |

Time-to-first-draw is ~20x faster (0.24 s vs ~4.98 s). Treat sampling
times as indicative rather than controlled: adaptation trajectories
legitimately differ between engines at matched seeds, so they measure
the whole run, not the gradient. (An earlier revision of the sampling
numbers was measured with a defective max tree depth of 5; the columns
in `docs/corpus-bench.tsv` and the table below have been re-measured at
the correct depth. The sampling columns for the 24 models whose
generated quantities run through the per-draw interpreter predate that
fallback, so they still understate CmdStan's side there.)

## Parallel chains, and what STAN_THREADS costs

Chains run in threads, one executor each. The tax worth measuring:
stan-math's autodiff stack becomes thread-local under `STAN_THREADS`,
and that indirection lands on every var operation, so the model to
worry about is one dominated by the nested-tape path.

| model | plain | `STAN_THREADS` |
| --- | ---: | ---: |
| 200-step `ordered_logistic` recurrence (72% in a legacy op) | 44,695 ns | 44,370 ns |
| `es` (eight schools, native kernels) | 221.4 ns | 223.0 ns |
| `ar1` | 228.9 ns | 224.1 ns |
| `conj` | 142.2 ns | 141.1 ns |

The tax is noise, in both directions, including on the model built to
expose it. Against that, eight chains of that model:

| threads | 1 | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| 8 chains, wall clock | 2.89 s | 1.59 s | 0.86 s | 0.49 s |

So it is on by default for native builds, and off under Emscripten
(wasm threads need a cross-origin-isolated page; the browser runs one
chain per worker instead).

**Threading does not change the answer.** Each chain owns its executor
and its RNG stream, so a parallel run is byte-identical to a sequential
one. Checked as a CSV `cmp` across four models at 8 chains x 300 draws,
and asserted in `tests/test_multichain.cpp` and `tests/test_python.py`.

One trap, worth naming because it is invisible from the outside:
stan-math's AD stack pointer is thread-local under `STAN_THREADS` and
starts **null** in every new thread. CmdStan never has to handle this
because TBB's scheduler hook instantiates a `ChainableStack` per
worker; this build stubs TBB out, so raw `std::thread`s must do it
themselves or segfault on the first legacy op.

## Numerical parity

Every model in the passing set is differentially verified against
CmdStan's `log_prob_propto_jacobian` and full gradient at the shared
point: 118/120 verified, 45 of them bitwise identical, worst relative
deviation 2.6e-12 (`tools/verify_sample.py`,
`docs/corpus-status.md`). For the 20 models whose generated quantities
are deterministic, the same oracle also replays CmdStan's write_array
values (every CSV column at the same point); recording those caught two
interpreter bugs invisible to structural coverage checks.

Transformed models change summation order relative to CmdStan's scalar
loop, so they verify at tolerance rather than bitwise: the passes
change 66 corpus models, and the worst gradient deviation any of them
introduces vs the untransformed graph is 6.0e-13 relative
(`harnesses/ab_corpus.py` compares every model passes-on vs passes-off
and flags any divergence). That harness caught an in-place rule that
made eight models silently wrong by up to 1.7e+05 relative with their
op counts unchanged; the six models most affected by the passes were
then also re-run through the CmdStan rig directly, and all six verify
(e.g. `radon_pooled` 2.1e-14, `arK` 9.6e-16, `rats_model` 2.4e-16).

## Full corpus

Every posteriordb model, sorted by per-gradient speedup. The columns
are CmdStan's absolute numbers and stanli's ratio against them.
Sampling is 1000 warmup + 1000 draws at matched seeds, indicative
rather than controlled; where the two columns disagree sharply, the two
engines sampled different modes. (`ldaK2` has two modes and the corpus
seed is the one seed of five where the engines split, stanli drawing
the mode that is five times more expensive to explore; that is the
whole of its 0.25x sampling column against a 0.71x gradient.
`hmm_gaussian`'s CmdStan run at that seed has every post-warmup draw
divergent, so its 18.75 s is a chain that is not sampling at all.) Read
the gradient column as the measurement. Regenerate with
`python3 tools/corpus_table.py docs/corpus-bench.tsv`.

| model | params | CmdStan ns/grad | grad speedup | CmdStan sample | sample speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| `radon_pooled` | 3 | 320,938 | 6.07x | 3.78 s | 5.82x |
| `logmesquite_logvash` | 7 | 2,841 | 5.46x | 0.39 s | 3.55x |
| `logmesquite_logvas` | 8 | 3,130 | 5.45x | 0.42 s | 3.82x |
| `Rate_2_model` | 2 | 561 | 5.45x | 0.23 s | 11.50x |
| `rats_model` | 65 | 6,475 | 5.29x | 0.52 s | 2.48x |
| `mesquite` | 8 | 3,055 | 5.25x | 0.57 s | 3.00x |
| `arK` | 7 | 12,459 | 5.23x | 0.97 s | 4.85x |
| `logmesquite_logva` | 5 | 2,070 | 5.10x | 0.29 s | 4.14x |
| `radon_hierarchical_intercept_centered` | 391 | 569,143 | 5.10x | 44.69 s | 5.02x |
| `radon_hierarchical_intercept_noncentered` | 391 | 570,300 | 5.09x | 56.03 s | 4.49x |
| `radon_county_intercept` | 388 | 431,614 | 4.81x | 27.37 s | 4.46x |
| `logmesquite` | 8 | 2,902 | 4.80x | 0.33 s | 4.12x |
| `GLM_Poisson_model` | 4 | 2,008 | 4.80x | 0.24 s | 3.43x |
| `radon_variable_intercept_centered` | 390 | 427,262 | 4.76x | 23.02 s | 4.50x |
| `radon_variable_intercept_noncentered` | 390 | 430,721 | 4.75x | 33.78 s | 4.17x |
| `logmesquite_logvolume` | 3 | 1,304 | 4.67x | 0.20 s | 6.67x |
| `radon_variable_slope_centered` | 390 | 420,987 | 4.66x | 23.66 s | 4.44x |
| `radon_variable_slope_noncentered` | 390 | 422,894 | 4.63x | 51.94 s | 4.65x |
| `kilpisjarvi` | 3 | 1,532 | 4.48x | 1.60 s | 1.86x |
| `Rate_1_model` | 1 | 260 | 4.41x | 0.15 s | 7.50x |
| `radon_partially_pooled_centered` | 389 | 272,243 | 3.94x | 13.96 s | 3.63x |
| `radon_partially_pooled_noncentered` | 389 | 273,685 | 3.92x | 20.11 s | 3.68x |
| `Rate_5_model` | 1 | 262 | 3.64x | 0.19 s | 9.50x |
| `Rate_3_model` | 1 | 268 | 3.62x | 0.18 s | 9.00x |
| `Rate_4_model` | 2 | 311 | 3.62x | 0.17 s | 8.50x |
| `nes` | 10 | 69,324 | 3.52x | 6.70 s | 4.14x |
| `radon_variable_intercept_slope_centered` | 777 | 437,889 | 3.50x | 27.23 s | 2.78x |
| `radon_variable_intercept_slope_noncentered` | 777 | 441,463 | 3.49x | 58.56 s | 3.18x |
| `seeds_centered_model` | 26 | 2,650 | 3.49x | 0.28 s | 2.80x |
| `sesame_one_pred_a` | 3 | 3,440 | 3.35x | 0.23 s | 4.60x |
| `surgical_model` | 14 | 1,684 | 3.32x | 0.23 s | 4.60x |
| `eight_schools_noncentered` | 10 | 745 | 3.28x | 0.20 s | 5.00x |
| `GLMM1_model` | 237 | 35,558 | 3.26x | 1.68 s | 1.56x |
| `kidscore_interaction_c` | 5 | 10,333 | 3.23x | 0.39 s | 4.33x |
| `seeds_stanified_model` | 26 | 2,341 | 3.22x | 0.30 s | 3.75x |
| `GLMM_Poisson_model` | 45 | 2,412 | 3.22x | 0.69 s | 3.83x |
| `kidscore_interaction_z` | 5 | 10,013 | 3.17x | 0.47 s | 3.62x |
| `kidscore_interaction` | 5 | 9,927 | 3.13x | 1.90 s | 2.68x |
| `kidscore_interaction_c2` | 5 | 9,901 | 3.13x | 0.40 s | 4.44x |
| `kidscore_mom_work` | 5 | 9,959 | 3.12x | 0.64 s | 4.00x |
| `election88_full` | 90 | 901,961 | 3.05x | 468.15 s | 3.16x |
| `logearn_interaction_z` | 5 | 26,484 | 2.90x | 0.84 s | 3.00x |
| `bym2_offset_only` | 3845 | 114,620 | 2.90x | 23.40 s | 1.46x |
| `dogs` | 3 | 63,747 | 2.90x | 2.39 s | 2.78x |
| `logearn_interaction` | 5 | 26,051 | 2.89x | 8.49 s | 2.38x |
| `seeds_model` | 26 | 2,130 | 2.89x | 0.29 s | 2.90x |
| `kidscore_momhsiq` | 4 | 7,145 | 2.86x | 0.84 s | 2.80x |
| `logearn_height_male` | 4 | 19,147 | 2.77x | 3.77 s | 2.24x |
| `logearn_logheight_male` | 4 | 18,697 | 2.71x | 13.41 s | 2.48x |
| `kidscore_momiq` | 3 | 4,861 | 2.57x | 0.42 s | 2.47x |
| `pilots` | 18 | 1,878 | 2.53x | 1.29 s | 3.49x |
| `logistic_regression_rhs` | 3075 | 113,106 | 2.52x | 16.23 s | 1.28x |
| `blr` | 6 | 1,728 | 2.44x | 0.21 s | 0.62x |
| `kidscore_momhs` | 3 | 4,483 | 2.43x | 0.30 s | 3.75x |
| `log10earn_height` | 3 | 11,560 | 2.34x | 1.75 s | 1.92x |
| `dugongs_model` | 4 | 1,653 | 2.28x | 0.25 s | 4.17x |
| `logearn_height` | 3 | 11,162 | 2.24x | 1.71 s | 2.34x |
| `earn_height` | 3 | 10,866 | 2.15x | 1.86 s | 2.21x |
| `GLM_Binomial_model` | 3 | 1,809 | 2.11x | 0.21 s | 3.50x |
| `dogs_log` | 2 | 41,387 | 2.07x | 1.01 s | 1.66x |
| `lsat_model` | 1006 | 91,173 | 2.00x | 4.66 s | 1.59x |
| `irt_2pl` | 144 | 37,468 | 1.86x | 2.17 s | 1.89x |
| `grsm_latent_reg_irt` | 408 | 762,133 | 1.68x | 66.92 s | 2.23x |
| `gpcm_latent_reg_irt` | 530 | 1,337,651 | 1.63x | 161.93 s | 2.68x |
| `wells_dist` | 2 | 39,202 | 1.62x | 1.44 s | 2.25x |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 26,320 | 1.53x | 39.76 s | 1.18x |
| `2pl_latent_reg_irt` | 531 | 134,556 | 1.51x | 7.94 s | 1.17x |
| `normal_mixture_k` | 14 | 357,439 | 1.42x | 101.61 s | 1.66x |
| `losscurve_sislob` | 15 | 3,450 | 1.41x | 0.31 s | 2.38x |
| `hierarchical_gp` | 933 | 47,565 | 1.35x | 17.20 s | 1.07x |
| `hier_2pl` | 669 | 397,603 | 1.30x | 26.82 s | 1.36x |
| `eight_schools_centered` | 10 | 314 | 1.29x | 0.19 s | 3.80x |
| `M0_model` | 2 | 15,595 | 1.29x | 0.37 s | 2.64x |
| `nes_logit_model` | 2 | 7,653 | 1.23x | 0.38 s | 2.53x |
| `prophet` | 62 | 69,789 | 1.23x | 117.68 s | 1.20x |
| `nn_rbm1bJ10` | 7951 | 185,731 | 1.14x | 456.82 s | 0.97x |
| `normal_mixture` | 3 | 88,239 | 1.12x | 1.13 s | 1.53x |
| `low_dim_gauss_mix_collapse` | 5 | 95,373 | 1.11x | 4.45 s | 1.22x |
| `low_dim_gauss_mix` | 5 | 98,315 | 1.11x | 1.98 s | 1.41x |
| `wells_dist100ars_model` | 3 | 18,997 | 1.09x | 0.62 s | 1.55x |
| `wells_dae_c_model` | 5 | 19,308 | 1.07x | 0.59 s | 1.59x |
| `wells_dist100_model` | 2 | 17,195 | 1.07x | 0.47 s | 1.81x |
| `wells_interaction_model` | 4 | 20,402 | 1.06x | 0.94 s | 1.27x |
| `wells_daae_c_model` | 6 | 20,885 | 1.06x | 0.63 s | 1.29x |
| `wells_dae_model` | 4 | 20,356 | 1.06x | 0.76 s | 1.43x |
| `wells_dae_inter_model` | 7 | 21,310 | 1.06x | 0.55 s | 1.67x |
| `gp_pois_regr` | 13 | 3,935 | 1.06x | 1.47 s | 1.20x |
| `wells_interaction_c_model` | 4 | 20,272 | 1.06x | 0.49 s | 1.81x |
| `Survey_model` | 1 | 61,578 | 1.03x | 1.14 s | 1.31x |
| `accel_splines` | 82 | 10,584 | 1.03x | 19.74 s | 0.86x |
| `dogs_hierarchical` | 2 | 34,053 | 1.03x | 0.68 s | 1.58x |
| `radon_county` | 389 | 82,076 | 0.99x | 4.49 s | 0.97x |
| `bones_model` | 13 | 51,501 | 0.98x | 1.31 s | 1.11x |
| `accel_gp` | 66 | 9,532 | 0.97x | 16.99 s | 1.01x |
| `Mth_model` | 394 | 93,922 | 0.97x | 5.43 s | 1.19x |
| `arma11` | 4 | 6,158 | 0.93x | 0.26 s | 2.17x |
| `dogs_nonhierarchical` | 65 | 40,588 | 0.91x | 2.86 s | 1.08x |
| `diamonds` | 26 | 31,497 | 0.89x | 48.55 s | 0.83x |
| `garch11` | 4 | 9,664 | 0.86x | 0.43 s | 1.34x |
| `Mh_model` | 388 | 38,956 | 0.85x | 2.66 s | 0.88x |
| `multi_occupancy` | 106 | 58,996 | 0.85x | 7.34 s | 0.97x |
| `gp_regr` | 3 | 4,698 | 0.83x | 0.23 s | 2.56x |
| `hmm_drive_1` | 6 | 147,829 | 0.82x | 6.94 s | 0.98x |
| `hmm_drive_0` | 6 | 132,850 | 0.77x | 3.65 s | 1.33x |
| `covid19imperial_v2` | 51 | 345,937 | 0.76x | 176.00 s | 0.79x |
| `covid19imperial_v3` | 51 | 342,943 | 0.76x | 175.70 s | 0.79x |
| `hmm_example` | 4 | 27,145 | 0.75x | 1.00 s | 1.05x |
| `one_comp_mm_elim_abs` | 4 | 470,681 | 0.74x | 11.23 s | 0.98x |
| `Mt_model` | 4 | 19,984 | 0.74x | 0.48 s | 1.33x |
| `ldaK2` | 7 | 104,059 | 0.71x | 3.19 s | 0.25x |
| `Mb_model` | 3 | 49,570 | 0.70x | 1.15 s | 0.86x |
| `hmm_gaussian` | 14 | 263,917 | 0.69x | 18.75 s | 0.03x |
| `soil_incubation` | 6 | 60,871 | 0.66x | 12.84 s | 0.56x |
| `iohmm_reg` | 29 | 320,335 | 0.59x | 181.23 s | 0.72x |
| `Mtbh_model` | 154 | 42,791 | 0.45x | 2.38 s | 0.62x |

120 models; 119 with both gradients; median per-gradient speedup 2.07x; 93/119 at or above CmdStan.

### The models the run could not complete

A missing number is not a slow number, so these sort below the table.

| model | params | CmdStan ns/grad | grad speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 7714 | 5,580,314 | 1.10x | both engines hit the 900 s sampling cap |
| `nn_rbm1bJ100` | 79411 | 434,981,254 | 1.07x | both engines hit the 900 s sampling cap |
| `kronecker_gp` | 438 | 217,990 | 0.70x | stanli sampling hit the 900 s cap (CmdStan takes 451 s) |
| `lotka_volterra` | 8 | 41,313 | 0.61x | stanli sampling hit the 900 s cap; untraced (CmdStan samples it in 6.2 s, so 0.61x per gradient should fit; `tools/sampler_trace.py` is the tool) |
| `sir` |  | - | - | the benchmark's fixed evaluation point is invalid: the ODE solution dips to -4.4e-10 and `poisson_lpmf` rejects the negative rate. CmdStan has no gradient number here either; the model samples fine. |

`ldaK5` and `nn_rbm1bJ100` are the two largest models in the corpus;
their per-gradient numbers are measured and stand.

## The browser build

Different compiler, different libm, so none of the numbers above carry
over. `tools/bench_wasm.cjs` measures it under Node against the same
MIR fixtures the tests use; `--mir`/`--data` point it at a bigger
model. Measure under Node, never in an automated browser tab: attaching
chrome.debugger keeps V8 on its baseline tier and costs about 4x.

### What the browser costs you

Both builds from the same commit, same MIR and data, min of three runs,
macOS arm64. This is the number the demo page's footer cites, and the
reason it tells people to install the wheel for real work.

| model | native | wasm | wasm/native |
|---|---:|---:|---:|
| `eight_schools_noncentered` | 269 ns | 534 ns | 1.98x |
| `radon_pooled` | 54.3 us | 106.6 us | 1.96x |
| `low_dim_gauss_mix` | 93.2 us | 168.4 us | 1.81x |
| `hmm_example` | 33.8 us | 58.6 us | 1.73x |
| `arK` | 2.32 us | 3.75 us | 1.62x |
| `lotka_volterra` | 74.6 us | 110.6 us | 1.48x |
| `diamonds` | 36.3 us | 52.2 us | 1.44x |
| geometric mean | | | **1.71x** |

The gap is gradient throughput, which is what dominates once sampling
starts. It is not model preparation: stanc3 compiled by js_of_ocaml
compiles eight schools in 3 ms warm, against 13 ms for the native
binary including process spawn.

### SIMD

SIMD128 is on. The A/B that decided it (macOS arm64, emsdk 6.0.6, on
the tree as it stood then; sizes have grown since as the density
surface roughly doubled):

| | scalar | +SIMD128 |
|---|---:|---:|
| fixtures, geomean | 457 ns | **449 ns** |
| `radon_pooled` (919 obs, vectorized normal) | 107.9 us | **105.7 us** |
| `nes` (matrix-heavy) | 29.1 us | **25.9 us** |
| `stanli.wasm` gzipped | 1.02 MB | 1.05 MB |

2% on most shapes and 11% on the matrix-heavy one for 0.03 MB, and
(the part that made it an easy call) every corpus model's gradients are
**bitwise identical** to the scalar build. With `-ffp-contract=off` in
force the vectorization Eigen takes is elementwise, and elementwise is
order-preserving; a vectorized reduction would have shown up as a
deviation immediately.

Two related results, measured: `-ffp-contract=fast` produces a
byte-identical `stanli.wasm`, because baseline WebAssembly has no FMA
instruction to contract to, so fast-math is not a lever on this target.
And splitting the densities into a lazily-loaded pack is viable but
blocked by one emscripten limitation; the measurements and traps are in
[docs/density-pack.md](density-pack.md).

## Reproducing

```
./tools/dev_setup.sh --corpus          # deps, build, posteriordb, CmdStan
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
python3 tools/bench_models.py deps/cmdstan deps/posteriordb
python3 harnesses/ab_corpus.py deps/posteriordb   # passes A/B over the corpus
```
