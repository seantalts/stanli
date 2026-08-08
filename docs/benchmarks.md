# Benchmarks vs CmdStan

2026-08-06, Apple M-series (macOS arm64), clang, both sides -O3 and
-ffp-contract=off, single-threaded. Both engines evaluate the sampling
gradient (propto + jacobian) at the same deterministic unconstrained
point. stanli runs `tools/bench_grad.cpp`; CmdStan runs
`tools/bench_cmdstan_grad.cpp` compiled against the stanc-generated
header, looping the same fresh-vars + grad + recover_memory cycle
`stan::model::gradient` performs per leapfrog step. Reproduce the whole
table with `tools/bench_models.py deps/cmdstan deps/posteriordb`.

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

(The table has changed shape twice: `radon_county`, `election88_full`
and `dogs` moved up with the write-fusion and constant-folding work, the
mixtures crossed parity with the elementwise-lp fusion, and `iohmm_reg`
climbed from 0.21x on the island pass. All three are described below.)

## Which models are faster, which are slower, and why

Across the full corpus (`docs/corpus-bench.tsv`, 119 models with both
gradients) the median per-gradient speedup is 2.07x and 93 of 119 models
are at or above parity. The ratio is almost entirely predicted by the
model's *shape*, not its size:

**Faster (most of the corpus, typically 1.5-6x):**

- **Vectorized-statement models.** A `y ~ normal(X * beta, sigma)` or a
  vectorized GLM over N observations is a handful of ops here; CmdStan
  builds and walks N var-tape nodes per statement per leapfrog step. The
  gap grows with N. This class is regressions, GLMs, and most
  hierarchical models written with vectorized statements.
- **Scalar loops the passes can vectorize.** The hierarchical indexing
  idiom -- `y[n] ~ normal(mu[county[n]], sigma)` and loops that fill a
  vector element by element -- arrives unrolled and is re-rolled back
  into the class above (radon family up to 6.1x, `election88_full`
  3.0x, `dogs` 2.8x). What the passes handle is described below.
- **Everything, on preparation.** Lowering a model is 4-400 ms against
  a ~7 s CmdStan compile, so short runs and iterative model development
  are dominated by this regardless of gradient speed.

**Near parity (0.8-1.2x):**

- **Models dominated by one large dense operation** -- a Cholesky, a big
  matrix product, an eigendecomposition (the GP models). Both engines
  spend their time inside the same stan-math kernel on the same
  contiguous doubles; interpreter overhead is noise on top.

**Slower (a shrinking tail, mostly 0.5-0.8x):**

- **Sequential models.** HMM forward recursions, state-space and
  ARMA/GARCH-style updates: each step reads the previous step's
  parameter-dependent result, so re-rolling correctly refuses
  (vectorizing a recurrence would change the math). The island pass
  (below) compiles such a region into one op, but only helps when the
  region's ops move real data: `iohmm_reg` copies a 1,500-element state
  vector per step and went 0.21x to 0.59x, while on the scalar
  recurrences the compiled form measured slower than the ops and the
  pass now declines it. What is left for them is per-op dispatch
  against CmdStan's inlined scalar code, and a native adjoint program
  is what would change that.
- **Mixture-shape models with K > 2 components** (`ldaK2`/`ldaK5`):
  their inner `log_sum_exp` runs over K-vectors built per document, a
  row-wise reduction the elementwise-lp fusion (below) does not yet
  express. The binary-mixture case is closed.
- **ODE models** were the extreme case (0.015x) when the right-hand
  side was interpreted per call; with it compiled (below) they sit at
  ~0.6x, the residue being our per-call dispatch against CmdStan's
  fully inlined right-hand side inside the same CVODES solver.
- **`Mtbh_model` (0.45x)**, now the slowest model, spends **36%** in
  `OP_SET_SLICE_STRIDED`: 146 calls moving 106,580 elements, most of it
  in the backward. The in-place rule below rewrites element writes
  (`OP_SET_INDEX`) but not slice writes, so a model that fills a matrix
  column by column still copies the whole matrix per column. Open.

A profile of every sub-parity model (`STANLI_PROFILE=1`, one gradient
each) is what the last two kernel changes came from, and it says the
remaining tail is mostly not a graph problem: in seven of those models a
single precompiled kernel is half to nine-tenths of the gradient.
`diamonds` was the extreme -- 90.9% in `OP_NORMAL_ID_GLM_LPDF`, whose
"native" kernel built a var tape in the forward, threw it away, and
built it again in the backward to call grad(). Differentiating once and
stashing the partials took it from 0.48x to 0.89x. `prophet` was 82% in
`OP_MATVEC`, where the accumulation was one serial dependency chain;
four independent accumulators took it from 0.67x to 1.23x, bitwise
unchanged. What is left in that class: `Mt_model` (62% in a scalar
`bernoulli_lpmf`), `gp_regr` (55% in `multi_normal_cholesky_lpdf`, where
the same partial-stashing measured a wash), `kronecker_gp` (38% in an
eigendecomposition).

**Where the wins come from: op granularity.** The interpreter's cost is
per op, not per element: ~17-20 ns for a scalar density op forward +
backward, measured as ~9.5 ns executor (dispatch + context assembly) plus
~9 ns recorder/sink inside the kernel, against ~0.9 ns for the actual
math (`tools/bench_opcost.cpp`). A vectorized statement over N elements
amortizes that to nothing and runs precompiled stan-math on contiguous
doubles, while CmdStan pays its var-tape cost per scalar per evaluation:
one tape node allocated, walked, and freed per leapfrog step, with
AoS-strided access. Vectorized models therefore win (2-6x, growing with
N), and models that were stuck as unrolled scalar loops used to lose
(0.4-0.9x before the re-roll pass).

**The re-roll pass** (`runtime/src/reroll.cpp`) closes that gap at the
graph level. Lowering unrolls data-bound loops, so a scalar-loop model
arrives as N consecutive copies of a small op template; the pass detects
these periodic regions and rewrites them into the vectorized ops the
kernels already support (constant vectors materialized from the const
pool, invariant ops hoisted, elementwise lanes widened, INDEX
progressions collapsed into their base vector, and N scalar density
terms fused into one summed vector density). `radon_pooled` collapses
from 27,670 ops to 8 (0.91x -> 6.18x) and `arK` from 3,164 to 21 (0.40x
-> 4.83x). Indexed reads rewrite by shape: the whole base in order needs
no op at all, a contiguous window becomes an `OP_SLICE`, and an arbitrary
data-driven index -- `alpha[county_idx[n]]`, the hierarchical idiom,
repeats and all -- becomes one `OP_GATHER` whose backward scatter-adds.
Anything the pass cannot prove safe it leaves alone, per region:
cross-lane recurrences, outputs escaping their lane, opcodes outside its
vocabulary. Set `STANLI_NO_REROLL=1` to disable it, `STANLI_NO_INPLACE=1`
for the update rules below.

**Element writes: `mu[n] = ...` inside a loop.** This is the other half
of the hierarchical idiom, and it used to be the worst thing in the
project. Each write lowered to a *functional update* -- copy the whole
vector into a fresh slot, poke one element -- so N writes cost O(N^2)
time
and O(N^2) arena. `radon_county_intercept` (N=12,573) spent 90.5 ms per
gradient inside 2.58 GB of arena, 207x slower than CmdStan.

Three rules compose to remove it (`runtime/src/inplace.cpp`, plus the
index rules above). A write may mutate its vector directly when it is the
**last use** of that vector -- not merely its only use, since the
read-back in the same iteration is an earlier use -- and when no earlier
reader needs the vector's values during the reverse sweep (`log_sum_exp`
and the other nested-replay backwards rebuild their tape from the input
buffer, so they must find it intact). The write and its read-back then
cancel outright, and when nothing else reads the vector its writes are
dead and swept. What is left is plain per-lane arithmetic, which the
gather rule vectorizes: **77,960 ops become 9**, 90.5 ms becomes 92 us,
2.58 GB becomes 42 MB. Seven radon-family models and `rats_model` collapse
the same way.

That worked when the read-back cancelled the write. When it did not --
when the loop fills a vector that something *else* reads, which is what
`y_hat[n] = a[county[n]]` followed by `y ~ normal(y_hat, sigma)` is --
the
writes survived, one op per element, and the re-roll pass refused the
region because its outputs escaped the lane. **Write-side fusion** takes
that case: a run of element writes marching contiguously through one
vector becomes a single vector store, and no store at all when the run
covers the vector, since the vectorized values can simply *be* it. Later
readers are redirected to the fused value. The conditions are what make
the redirection sound rather than what makes it possible: the vector must
be the same one every lane, no one else may read it while it is
half-written, nothing may write it after the run, and it must not be read
from outside the graph. `radon_county` goes from 25,152 ops to **10**
(0.36x -> 0.98x of CmdStan) and `election88_full` from 289,165 to **65**
(0.39x -> 2.97x). Eleven more radon-family variants collapse to 10-22 ops
each. 57 of the 120 corpus models now change under the passes, against 28
before.

Three follow-ons closed the `dogs` family (0.65x -> 2.8x, 12,751 ops ->
261). A **strided** run -- indices advancing by the matrix's row count,
`p[j, t]` filled down columns -- fuses into `OP_SET_SLICE_STRIDED`, and
interleaved runs over one vector chain block by block: each block's store
output becomes the vector every later reference, read or write, is
renamed to, so the next block fuses onto it in turn. **Per-lane integer
outcomes** fuse too: an lpmf lane carries its observation as an
immediate, so the lanes match as a template up to that immediate and the
fused vector op's outcome array is just their concatenation (the vector
kernels already take outcomes exactly that way). And the **unary math
ops** (exp, log, inv_logit, sqrt, ...) joined the widening vocabulary,
since their kernels were already shape-dispatching on `out.len`.

**Mixture fusion: the elementwise-lp variant.** A mixture model's
per-observation shape is `log_mix(theta, normal_lpdf(y[n]|...),
normal_lpdf(y[n]|...))`: the density outputs feed an op instead of the
target, so the summed-density fusion above cannot apply. Three pieces
close it. Densities gained a variant bit meaning *elementwise*: the
fused op's output is a vector holding each lane's own lp, computed by
the same per-element recorder call the scalar lanes used (bit-identical,
stan-math computing every value and partial). `log_mix` and the
two-argument `log_sum_exp` kernels batch over lanes with the usual
broadcast shape dispatch. And the re-roll pass classifies a density
whose lane outputs are consumed only inside their own lanes as
elementwise, widens the consuming `log_mix`, and swaps the N per-lane
target terms for one `OP_SUM_VEC`. `low_dim_gauss_mix` drops from 7,208
ops to 16 and crosses parity (0.78x -> 1.07x); `normal_mixture` lands at
13 ops, 1.09x. A density whose inputs are all lane-invariant hoists to
one scalar op instead of widening -- a len-N output over all-scalar
inputs is exactly the miscompile shape the corpus A/B caught once
already (`losscurve_sislob`).

**Tape islands: the irreducible residue.** After every other pass has
run, whatever scalar residue survives is, by construction, what no
vectorizer can help -- cross-lane recurrences, where step t reads step
t-1's parameter-dependent result. The island pass
(`runtime/src/island.cpp`, `STANLI_NO_ISLAND=1` to disable) compiles
each maximal run of compilable ops into a flat register program executed
by ONE op: forward runs it on plain doubles; backward replays it under
stan-math's nested autodiff and harvests the live-ins' adjoints -- the
same var arithmetic CmdStan's generated code runs for the same
statements, so gradients match by construction. Per-lane data constants
absorb into the program as immediates; dead copy-then-modify chains
reuse their base's registers (on `iohmm_reg` that is the difference
between 1.6M registers and 94k).

The op collapse is dramatic on every model that has such a region, and
the time follows on exactly one. Measured with `STANLI_NO_ISLAND=1` as
the baseline, same build, same point, on all fourteen corpus models that
compile a region:

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

The op count was never the cost. A dispatch is ~5 ns and the scalar
kernels around it are cheap; a var replay of the same operations costs
what CmdStan costs for them, which is more than the scratch-partials
backwards it replaced. `iohmm_reg` is different for a reason that has
nothing to do with dispatch: its steps copy a 1,500-element state
vector each, 1.6M elements of traffic per gradient, and the island's
registers make those copies disappear. `bones_model` is the same
mechanism inverted -- 36 ops behind a 4,024-register file, rebuilt as
vars 77 times per gradient.

So the pass estimates both sides before committing: what the ops move
(an in-place element update moves one element, not a vector) against
the register file, weighted 4x because it is built twice per call and
once as vars. The estimate separates the fourteen exactly -- `iohmm_reg`
1.6M against 435k, every other region 4-20x the wrong way -- and the
thirteen it now leaves alone are bitwise identical to the passes-off
baseline again. `STANLI_ISLAND_ALWAYS=1` skips the estimate, which is
how to ask why a region was left alone.

What would move the rest of this class is not fewer dispatches but a
cheaper backward: a native adjoint program, generated alongside the
forward one instead of replayed under nested autodiff. Beyond the
estimate, islands also refuse propto densities (their term-dropping
depends on argument types, which the island's uniform binding cannot
reproduce), runs under 32 ops, and regions producing target terms.

**Dispatch, measured to its floor.** The executor's sweeps resolve their
function pointers at bind time and run 4x-unrolled
(`tools/bench_dispatch.cpp` holds the comparison: a musttail-chained
alternative measured slower and noisier than the unrolled loop, so it
was declined). After those, per-op cost is bound by the context loads,
not the dispatch branch: the remaining lever for op-heavy graphs is
fewer ops, which is what the passes above are.

**ODE models: the right-hand side was an interpreter.** Every user function
is inlined at lowering time except one -- an ODE right-hand side has to stay
callable at runtime, because the integrator picks the times. It was evaluated
by a tree-walking interpreter over the MIR, at a `std::map` lookup per
variable reference and a `std::vector` allocation per intermediate: 5.8 us per
call for lotka_volterra's two-line right-hand side, ~500 calls per gradient,
**97% of the model's gradient time**. It also solved the system twice per
gradient, once for the values and again for the derivatives.

Both are gone. The right-hand side compiles once, at lowering time, into a
flat register machine (`runtime/src/ode_prog.cpp`): names become indices,
loops over the states unroll, data-only conditions fold, conditions on the
solve time become branches, and a call is a switch over a contiguous
instruction array with no allocation. And the forward sweep, which has to
solve the coupled state-plus-sensitivity system anyway to match CmdStan's
step control, now keeps the sensitivities instead of throwing them away, so
the backward is a matrix-vector product.

| model | before | after | speedup | vs CmdStan |
| --- | ---: | ---: | ---: | ---: |
| `lotka_volterra` | 2,790,941 ns | 71,704 ns | 38.9x | 0.015x -> 0.58x |
| `soil_incubation` | 3,389,538 ns | 96,362 ns | 35.2x | 0.018x -> 0.63x |
| `one_comp_mm_elim_abs` | 18,873,857 ns | 653,181 ns | 28.9x | 0.025x -> 0.74x |

Gradients are unchanged to the bit where they were before, and
`lotka_volterra` moved from 4 ULP to bitwise identical to CmdStan: reading the
jacobian out of the same solve that produced the values removes a second,
independently stepped solve. All four corpus models that call
`integrate_ode_*` compile their right-hand side. Anything the compiler cannot
express -- a `return` out of a branch on the solve time, say -- keeps the
interpreter, so coverage never shrinks; `STANLI_DEBUG_ODE=1` reports when that
happens, since a silent 30x is worth a line.

(`one_comp_mm_elim_abs` has a CmdStan number here for the first time.
It is the only corpus model calling `integrate_ode_bdf`, which reaches
CVODES, and the gradient driver linked only TBB -- so the comparison
failed to build and the row went blank with nothing to say why. The
harness now links the CVODES archives and names the failure if a driver
still will not build or run.)

Model preparation scales too: the largest model in the corpus
(`nn_rbm1bJ100`, MNIST, 60,000 rows, 79,411 parameters) lowers to a
192,030-op graph in 20.7 s and evaluates its gradient in 0.43 s. That
number used to be unbounded: the transformed-data interpreter evaluated
an indexed expression by copying its base, so reading `y[n]` in a loop
copied the whole array each time and lowering was quadratic in the data
size.

Preparation time is the other axis: stanli lowers a model in 4-200 ms,
against a 6.2-7.6 s CmdStan compile (with a warm precompiled header, and
after a multi-minute one-time `make build`). That gap is what
time-to-first-draw is made of. Re-rolling also cut preparation time on
loop-heavy models (radon_pooled 0.39 s -> 0.08 s): the executor binds
and sizes 8 ops instead of 27,670.

## End to end: eight schools, model.stan + data.json -> 1000 warmup + 1000 draws

An earlier revision of these sampling numbers was measured with a
defective max tree depth of 5 (`stan::mcmc::base_nuts` defaults to 5;
CmdStan sets 10, and `run_nuts` never called `set_max_depth`), capping
trajectories at 31 leapfrogs and understating any model that needs deep
trees. The sampling columns in `docs/corpus-bench.tsv` and the full
table below have since been re-measured at the correct depth; treat them
as indicative rather than controlled, since adaptation trajectories
legitimately differ between engines at matched seeds.

They were also unequal work in a second way: stanli computed no
transformed parameters and no generated quantities, so on the models with
a generated quantities block (`diamonds`, `accel_splines`,
`covid19imperial_v2` among the biggest apparent sampling wins) CmdStan
was doing per-draw work stanli skipped. That gap is closed for all 119
compiling models: the write_array graph covers 95, and the 24 the graph
cannot express (RNG draws, branches on draw-computed values) run through
a per-draw interpreter (`harnesses/wa_coverage.py` is the sweep). The
sampling columns for those 24 predate the interpreted fallback, so they
still understate CmdStan's side there.

| engine | stage | time |
| --- | --- | --- |
| stanli | stanc + graph compile + bind | 0.014 s |
| stanli | NUTS 1000+1000 (incl. constraining draws) | 0.020 s |
| stanli | CLI total (`stanli_run`, process start to CSV) | 0.24 s |
| CmdStan | model build (stanc + clang, warm PCH) | 4.62 s |
| CmdStan | NUTS 1000+1000 (self-reported total) | 0.044 s |
| CmdStan | build + run wall time | ~4.98 s |

Time-to-first-draw is ~20x faster (0.24 s vs ~4.98 s). Sampling-phase
times are dominated by gradient cost, and adaptation trajectories differ
between engines (CmdStan took 9,654 post-warmup leapfrogs here), so treat
the sampling rows as indicative; the controlled comparison is the
per-gradient table.

## Parallel chains, and what STAN_THREADS costs

Chains run in threads, one executor each. The question worth measuring
was not the speedup but the tax: stan-math's autodiff stack is a plain
static unless `STAN_THREADS` is defined, in which case it becomes
thread-local (`__thread` on gcc/clang), and that indirection lands on
every var operation. So the model to worry about is not a vectorized one
-- native kernels never touch the var stack -- but one dominated by the
nested-tape path.

| model | plain | `STAN_THREADS` |
| --- | ---: | ---: |
| 200-step `ordered_logistic` recurrence (72% in a legacy op) | 44,695 ns | 44,370 ns |
| `es` (eight schools, native kernels) | 221.4 ns | 223.0 ns |
| `ar1` | 228.9 ns | 224.1 ns |
| `conj` | 142.2 ns | 141.1 ns |

Best of three each. The tax is noise, in both directions, including on
the model built specifically to expose it. Against that, eight chains of
that same model:

| threads | 1 | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| 8 chains, wall clock | 2.89 s | 1.59 s | 0.86 s | 0.49 s |

So it is on by default for native builds, and off under Emscripten (wasm
threads need `-pthread` and a cross-origin-isolated page; the browser
build runs one chain per worker instead).

**Threading does not change the answer.** Each chain owns its executor --
its own arena, its own contexts -- and its own RNG stream, so a parallel
run is byte-identical to a sequential one. Checked as a CSV `cmp` across
four models (eight schools, the `ordered_logistic` recurrence, `ar1`, and
`conj` with its generated quantities), 8 chains x 300 draws each, and
asserted in `tests/test_multichain.cpp` and `tests/test_python.py` in a
form that holds on a single-threaded build too.

**One trap, worth naming because it is invisible from the outside.**
stan-math's AD stack pointer is thread-local under `STAN_THREADS` and
starts **null** in every new thread; each child thread must instantiate a
`ChainableStack` before touching the AD system, which stan-math's own
header documents and which CmdStan never has to write because TBB's
scheduler-entry hook (`ad_tape_observer`) does it for every worker. This
build stubs TBB out, so raw `std::thread`s dereferenced null inside
`start_nested()` on the first legacy op. A segfault rather than a wrong
number, which is the good version of this bug.

## Numerical parity

Every model in the passing set is differentially verified against
CmdStan's `log_prob_propto_jacobian` and full gradient at the shared
point: 118/120 verified, 44 of them bitwise identical, worst relative
deviation 2.6e-12 (`tools/verify_sample.py`, `docs/corpus-status.md`).
For the 20 models whose generated quantities are deterministic
(kronecker_gp sits out with its documented eigenvector deviation), the
same oracle also replays CmdStan's write_array values (every CSV column at the
same point); recording those caught two interpreter bugs on its first
run, an uninitialized-value semantic and a transposed batched-simplex
read, both invisible to structural coverage checks.
Transformed models change summation order relative to CmdStan's scalar
loop, so they verify at tolerance rather than bitwise: across the corpus
the passes now change 66 models and the worst gradient deviation any of
them introduces vs the untransformed graph is 6.0e-13 relative --
`iohmm_reg`, whose entire forward algorithm replays through one island
(`harnesses/ab_corpus.py` compares every corpus model passes-on vs
passes-off and flags any divergence; `--disable` one variable to
attribute one).

That harness earns its keep. An earlier version of the in-place rule
allowed a destructive write whenever it was the last use of its vector,
which is wrong for any earlier reader that rebuilds its var tape from the
buffer during the reverse sweep -- `log_sum_exp`, `softmax`, every
legacy
nested-replay backward. Eight HMM/LDA/mixture models were silently wrong
by up to 1.7e+05 relative **with their op counts unchanged**, so nothing
structural would have caught it. Only ops whose backward purely routes
adjoints may now precede a destructive write.

All six were then re-run through the CmdStan rig directly rather than
resting on that transitive argument, and all six still verify:
`radon_pooled` 2.1e-14 (140 ulp, against 135 before the pass), `arK`
9.6e-16 (5 ulp, against 1), `rats_model` 2.4e-16 (2 ulp),
`soil_incubation` 1.3e-16 (1 ulp), and both `covid19imperial` variants
8.2e-16 (7 ulp), unchanged.

## Full corpus

Every posteriordb model, sorted by per-gradient speedup. The columns are
CmdStan's absolute numbers and stanli's ratio against them: the ratio is
what the table is read for, and stanli's own time is the ratio applied to
the column beside it. Sampling is 1000 warmup + 1000 draws at matched
seeds, indicative rather than controlled -- the two samplers take
different trajectories, so it measures the whole run, not the gradient.
Where the two columns disagree sharply, the two engines sampled
different modes. `ldaK2` has two (mean lp about -596 and about -696) and
the second is five times more expensive to explore -- adapted stepsize
0.09 against 0.48. Over five seeds through `tools/sampler_trace.py` the
engines picked the same mode in four and the corpus seed is the fifth,
where stanli drew the expensive mode and CmdStan the cheap one. That is
the whole of its 0.25x sampling column against a 0.71x gradient, and it
is a property of the posterior, not of either sampler. `hmm_gaussian`
(0.69x per gradient, 0.03x sampling) is the same shape with a sharper
edge: at that seed CmdStan's run has *every* post-warmup draw divergent,
so its 18.75 s is a chain that is not sampling at all. Read the sampling
column as indicative and the gradient column as the measurement. (It is
also older than the gradient column in one way: it was measured before
the sampler moved to CmdStan's generator, so the seed that produced each
row's draws is not the seed that would produce them today.)
Regenerate with `python3 tools/corpus_table.py docs/corpus-bench.tsv`.

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

A missing number is not a slow number, so these sort below the table
rather than inside it.

| model | params | CmdStan ns/grad | grad speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 7714 | 5,580,314 | 1.10x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `nn_rbm1bJ100` | 79411 | 434,981,254 | 1.07x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `kronecker_gp` | 438 | 217,990 | 0.70x | stanli sampling hit the 900 s cap |
| `lotka_volterra` | 8 | 41,313 | 0.61x | stanli sampling hit the 900 s cap |
| `sir` |  | - | - | stanli's gradient probe threw at the benchmark point; no stanli gradient |

- `ldaK5` and `nn_rbm1bJ100` are the two largest models in the corpus
  (7,714 and 79,411 parameters; CmdStan itself needs 435 ms per gradient
  on the second one). Neither engine finishes 2000 iterations inside the
  cap. Their per-gradient numbers are measured and stand.
- `kronecker_gp` is 0.70x per gradient and CmdStan takes 451 s, so the
  same run does not fit in 900 s.
- `lotka_volterra` is the honest puzzle left: CmdStan samples it in
  6.2 s and stanli is 0.61x per gradient, so the cap should be nowhere
  near. The mode explanation above covers `ldaK2` and `hmm_gaussian`;
  this one has not been traced yet. `tools/sampler_trace.py` is the tool.
- `sir` throws at the benchmark's fixed evaluation point: the ODE
  solution dips to -4.4e-10 and `poisson_lpmf` rejects a negative rate.
  CmdStan has no gradient number here either. The model samples fine
  (38.7 s in CmdStan), so this is the probe point, not the model.

## The browser build

Different compiler, different libm, and until now no SIMD, so none of the
numbers above carry over. `tools/bench_wasm.cjs` measures it under Node
against the same MIR fixtures the tests use (no stanc, no posteriordb
needed); `--mir`/`--data` point it at a bigger model.

```
node tools/bench_wasm.cjs                      # fixtures, ns/gradient
node tools/bench_wasm.cjs --module build-wasm-simd/stanli.js
```

The A/B that turned SIMD on, measured on macOS arm64, emsdk 6.0.6 (the
version CI pins), on the tree as it stood at that commit:

| | scalar | +SIMD128 |
|---|---:|---:|
| fixtures, geomean | 457 ns | **449 ns** |
| `radon_pooled` (919 obs, vectorized normal) | 107.9 us | **105.7 us** |
| `nes` (matrix-heavy) | 29.1 us | **25.9 us** |
| `stanli.wasm` gzipped | 1.02 MB | 1.05 MB |

The absolute numbers have moved since -- the density surface roughly
doubled -- and today's build is 4.10 MB raw, 1.15 MB gzipped, 478 ns
geomean. The A/B above is kept as the decision record; rerun it against a
freshly configured scalar build if the decision is ever revisited.

SIMD128 is on. It is worth 2% on most shapes and 11% on the matrix-heavy
one for 0.03 MB, and -- the part that made it an easy call -- every corpus
model's gradients come out **bitwise identical** to the scalar build
(`tools/wasm_check.sh` against the CmdStan references, plus a direct
comparison on `radon_pooled` and `nes` at three points each). With
`-ffp-contract=off` still in force the vectorization Eigen takes is
elementwise, and elementwise is order-preserving. A vectorized reduction
would reassociate, and would have shown up as a deviation immediately.

### Two things not worth doing, measured

**`-ffp-contract=fast` is a no-op here.** Not "not worth it" -- it produces
a **byte-identical** `stanli.wasm`. Baseline WebAssembly has no FMA
instruction, so there is nothing to contract (`f64x2.relaxed_madd` needs
the relaxed-SIMD proposal and `-mrelaxed-simd`). The `-ffp-contract=off`
pin that costs something on native costs exactly nothing in the browser,
and fast-math is not a lever on this target at all.

**Splitting densities into a lazily-loaded pack is viable, and the
obstacle I expected is not there.** Stubbing every density, cdf and tail
kernel and relinking says where the payload actually is:

| | raw | gzip |
|---|---:|---:|
| core runtime (plus `multi_normal`, `lkj_corr_cholesky`) | 2.26 MB | **0.69 MB** |
| everything, as shipped | 4.10 MB | **1.15 MB** |

So the density surface is 1.84 MB raw, **0.46 MB gzipped** -- 40% of the
payload, against a 0.69 MB core that cannot be split. A model still needs
its own densities, so the realistic saving is smaller than 0.46 MB, but it
is no longer marginal: this doc previously said 0.37 MB against a bigger
core, before the density list went from 47 to 71.

The cost I assumed would eat it does not exist. Emscripten's
`MAIN_MODULE=2` with `-fPIC` was measured against the same tree: **1.05 MB
gzipped either way, 451 ns against 449 ns**. On wasm, calls already go
indirect through a function table, so there is no PLT/GOT penalty of the
kind native dynamic linking pays.

The mechanism was proven end to end with a spike: a `SIDE_MODULE` built
against these headers, loaded at runtime, calling **back into the main
module** -- which is what lets a pack call `register_kernel`,
`active_sink()` and libm from the core instead of carrying its own copies.
Dispatch is already `kernel(opcode).forward`, a function-pointer table, so
late registration only has to fill in slots, and the lowering already
fails with `unsupported function <name>` -- so the trigger is
self-correcting: try to lower, fetch the pack on that error, retry, with
no density-name table duplicated in JavaScript.

One requirement, found the hard way: the pack must be compiled with the
same exception ABI (`-fwasm-exceptions`) as the core. Mismatched, it fails
with an opaque `__stack_pointer` mutable-global import error that looks
like a dynamic-linking bug and is not.

## Reproducing

```
./tools/dev_setup.sh --corpus          # deps, build, posteriordb, CmdStan
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
python3 tools/bench_models.py deps/cmdstan deps/posteriordb
python3 harnesses/ab_corpus.py deps/posteriordb   # re-roll A/B over the corpus
```
