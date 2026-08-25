# Graph optimizations and performance work

What stanli does to a model's op graph before running it, written for
people who have not worked on compilers. Each section says what the
problem is, what the pass does, and how to turn it off when debugging.
Measurements are in [`docs/benchmarks.md`](../../docs/benchmarks.md).

## Data input preload (`lower.cpp`, disable: `STANLI_NO_DATA_PRELOAD=1`)

stanc's transformed MIR contains a small program that reconstructs each input
from a flat `FnReadData` buffer. That is useful to generated C++, but redundant
here: `DataMap` has already parsed the JSON into the same typed, column-major
layout. Replaying the program is especially expensive for matrices because it
performs one interpreted assignment per element.

Lowering therefore binds declared inputs directly and recognizes the statement
and name envelope used by stanc's pinned hydration code: a direct
scalar/vector load, or the generated flat-buffer container-copy loop. Anything
with another statement kind, effect, data source, or written name still goes
through the transformed-data interpreter. User-written transformed-data
statements always run; `STANLI_NO_DATA_PRELOAD=1` remains the exact interpreter
oracle for auditing future stanc output changes.

On `nn_rbm1bJ100`, whose MNIST input is a 60,000 x 784 matrix, this reduces the
`bind_data` stage from 23.56 s to 0.09 s and complete graph compilation from
23.80 s to 0.23 s. It also avoids carrying an integer mirror merely because
the JSON happened to spell a real matrix with integer tokens. Set
`STANLI_NO_DATA_PRELOAD=1` to replay the generated hydration as a correctness
oracle.

## Background: why op count matters

stanli turns a Stan model into a list of operations ("ops"). Each op
reads some buffers, computes something, and writes a buffer. The log
density is the list run top to bottom; the gradient is the list run
bottom to top.

Running one op costs roughly 15-20 nanoseconds regardless of how small
the computation inside it is. An op that adds two numbers pays the same
fixed cost as an op that adds two vectors of 10,000 numbers. So the
main way to make a model faster is to make the list shorter: replace
many small ops with few big ones.

The problem is that lowering unrolls loops. A model with

```stan
for (n in 1:1000) {
  y[n] ~ normal(mu[county[n]], sigma);
}
```

becomes several thousand single-number ops. The passes below undo that
without changing the numbers the model computes. They run in this
order, from `lower.cpp`:

1. in-place updates (`inplace.cpp`)
2. store-to-load forwarding (`inplace.cpp`)
3. constant folding (`constfold.cpp`)
4. loop re-rolling (`reroll.cpp`)
5. in-place updates again, for slice stores created by re-rolling
6. tape islands (`island.cpp`)

## In-place updates (`inplace.cpp`, disable: `STANLI_NO_INPLACE=1`)

Writing one element of a vector, like `mu[n] = x;` in a loop, lowers as
a "functional update": copy the whole vector into a new buffer, then
change one element. N writes into a length-N vector then copy about
N*N/2 numbers. One real model (radon_county_intercept, N = 12,573)
spent 90 ms per gradient and 2.6 GB of memory this way.

The fix: if nothing ever looks at the old version of the vector again,
change the element directly in the existing buffer. Re-rolling can create
contiguous or strided slice stores only after the first in-place pass, so the
same proof runs again after re-rolling. "Nothing looks at it again" has two
parts:

- No later op reads the old version; the write is the last use of that
  buffer.
- No earlier op needs the old values during the gradient step. Some
  ops recompute their derivative from their input buffer when the
  gradient runs, which happens after all the writes. Only ops whose
  gradient step just moves numbers around, without re-reading input or output
  values, may sit before an in-place write. There is an explicit list
  of those ops (`backward_ignores_values`) and a test that checks
  each one really has that property.

The second condition was learned the hard way: without it, eight models
were silently wrong by large amounts with nothing visibly different
about their graphs. The full-corpus comparison at the end of this file
is what caught it.

Every chain keeps one copying write when its base is a declaration fill rather
than an op result. That copy restores the full buffer on every evaluation;
later element or slice writes can share its fresh output. In reverse, untouched
slice cells pass through the aliased base/output adjoint buffer automatically.
Overwritten cells are routed to the slice RHS and then cleared from that shared
buffer, so an earlier write cannot count them again. Direct base/RHS aliases,
malformed ranges, roots, parameters, and bases without a value-free producer
before the write all remain copying.

On `Mtbh_model`, re-rolling creates 146 strided stores of four values into a
730-cell matrix. Re-running this pass leaves the initial full copy and turns
all 146 stores into four-cell updates, reducing median gradient latency from
106.5 to 47.4 us (2.24x).

## Store-to-load forwarding (`inplace.cpp`, same switch)

A common pattern: write `mu[n]`, then read `mu[n]` back in the same
iteration. After lowering that is a write op immediately followed by a
read op on the same element. The pass deletes the read and hands the
value straight to its consumer. If nobody reads the vector at all after
this, the writes are deleted too. This turns "loop that fills a vector"
into "loop that does arithmetic", which re-rolling knows how to
vectorize.

## Constant folding (`constfold.cpp`, disable: `STANLI_NO_CONSTFOLD=1`)

Some parts of a model do not depend on the parameters, but still lower
to ops that recompute the same numbers on every gradient evaluation.
The pass finds every op no parameter can influence, runs those ops
once, saves the results, and deletes the ops.

To run them "once", the pass builds a small temporary graph from just
the constant ops and runs it through the normal executor. This is
deliberate: the kernel is the only definition of what an op computes,
and a separate evaluation path could disagree with it.

One ordering rule: folding replaces a buffer with its final contents.
If a surviving op reads the buffer at a moment when it held different,
earlier contents (this happens with in-place write chains), that buffer
cannot be folded, and nothing computed from it can be either.

## Loop re-rolling (`reroll.cpp`, disable: `STANLI_NO_REROLL=1`)

The main pass. An unrolled loop shows up as the same short op pattern
repeated once per data point; each repetition is called a lane. If the
pass can prove the lanes independent, it replaces them with a handful
of vector ops. To do that it works out how each input varies from lane
to lane:

- Same buffer every lane: keep it; the vector kernels broadcast a
  scalar across elements.
- A different constant every lane: collect the constants into a new
  vector loaded at setup time.
- The output of an earlier op in the same lane: use that op's
  vectorized output.

Element reads and writes get special handling:

- Reading `v[0], v[1], ...` in order across the whole vector: no op at
  all; consumers read `v` directly.
- Reading the same element every lane: one hoisted copy.
- Reading a contiguous range: one slice op.
- Reading arbitrary elements, like `mu[county[n]]`: one gather op. Its
  gradient step adds each element's contribution back to the right
  place, including repeated elements.
- Writing `v[0], v[1], ...` across lanes: one store op, or no op when
  the writes cover the whole vector (the computed vector simply becomes
  `v`). Writes stepping by a fixed stride (filling a matrix column by
  column) use a strided store. When several write runs take turns
  filling one vector, each run's store output becomes the vector
  everything later refers to, so the runs chain and each vectorizes on
  its own.
- A run of writes that all write the same value: keep the computation
  scalar and broadcast it across the range.

Densities get two more rewrites, depending on where their results go.
If every lane's density result goes straight into the target, N scalar
density ops become one vector density op (the vector kernels already
return the summed density). Discrete densities carry their integer
outcome as an attached immediate; lanes fuse by concatenating those
immediates, the form the vector kernel expects. If instead every lane's
density result feeds another op in the same lane (the mixture idiom,
where two densities feed a `log_mix`), the N scalar ops become one
*elementwise* density op whose output holds each lane's own log
density, computed by the same per-element call the scalar ops used, so
the results are bit-identical. The consumers (`log_mix`,
two-argument `log_sum_exp`, ordinary arithmetic) then vectorize as
usual, and one summing op replaces the N per-lane target entries. A
density whose inputs are the same buffer in every lane stays one scalar
op instead.

Fixed-width row reductions get one deliberately narrower pre-pass. The LDA
shape fills every element of a short `gamma` vector, then contributes
`log_sum_exp(gamma)` once per document. Because K=2 is below the ordinary
lane threshold and the scalar reduction separates the outer rows, the exact
concrete grammar is recognized as a unit: two indexed reads, logs, an add,
the complete element stores, and a target-only reduction. Consecutive rows
become two gathers over packed row-major indices, vector logs/add, one
`OP_LOG_SUM_EXP_ROWS`, and `OP_SUM_VEC`. Every removed temporary and the
row scratch buffers must have only the proven readers/writers and no external
root. Rows may continue one shared in-place chain or start from disjoint fresh
scratch, but every accepted row overwrites indices 0 through K-1 before its
reduction. Target terms must be one unique contiguous subsequence. Repeated
gather indices are safe, while partial stores, recurrences, escaped values, or
mixed bases refuse the rewrite.

Anything the pass cannot prove safe it leaves alone. The common
reasons: a lane reads a result computed by the previous lane (a
recurrence involving parameters), a lane's intermediate is used outside
the loop, or an op it has no rule for. When a pattern only partly
qualifies, the pass rewrites the longest qualifying prefix of lanes and
tries again on the rest, which handles data that comes in blocks.

Scale: `radon_pooled` drops from 27,670 ops to 8, `radon_county` from
25,152 to 10, `election88_full` from 289,165 to 65, `dogs` from 12,751
to 261, `low_dim_gauss_mix` to 16.

Candidate discovery is indexed once before classification. A period window can
therefore ask in O(1) whether it contains any density, element store, or
target-producing widenable op; a graph with none returns before allocating the
reader/writer lists. This matters for large scalar residue the pass cannot
express: `nn_rbm1bJ100`'s zero-region scan falls from 126 ms to 13 ms, and the
candidate-free million-op LDA shape from 1.16 s to about 3.4 ms. The exact
candidate-index, packed-row, and use-list work counters live in `RerollStats`
and have deterministic scaling tests.

## Lane partitioning (`partition.cpp`, disable: `STANLI_NO_PARTITION=1`)

Re-rolling asks whether a template repeats with period P, which requires
the repetitions to be adjacent and the scan to arrive in phase. Neither
holds as often as the shape of the source suggests:
`state_space_stochastic_level_stochastic_seasonal` contains one perfect
four-op random walk that re-rolling leaves entirely scalar, because the
window-sum loop ahead of it leaves the period scan two ops off phase.
This pass asks a different question: where does a lane end?

A lane ends at a target term or at an element store, and reaches back
over the ops whose values never leave it. Lane bounds are therefore
computed rather than discovered, and lanes need not be adjacent. They are
grouped by a structural fingerprint (opcodes, shapes, dataflow edges, and
the shapes of immediates, never which slot a lane reads), which is what
lets the two branches of a data-dependent condition share one bucket, and
each bucket is rewritten in place of its first lane.

What a bucket's ops read is proven rather than assumed: no slot the fused
ops read from outside may be written while the run is in flight. That one
rule carries three soundness obligations at once (no cross-lane
recurrence, no writer in the middle of the bucket, no destructive
in-place store between the lanes), because all three appear as a writer
between the first lane's position and the last lane's end. Ops only ever
move earlier, so an in-place store already proven safe against a later
reader stays safe, and a bucket that trips the rule splits at the writer
rather than declining.

A bucket is rewritten in one of these forms:

- One template, one bucket: each input becomes a slice when the lanes
  read a contiguous range, a gather when they do not, and a shared
  operand when every lane reads the same slot. Contiguity is a cost
  question here, not a legality one.
- Density lanes whose outcomes ride along as immediates concatenate
  those immediates into the layout the vector kernel unpacks, group by
  group for the binomial family's `n`/`y` pairs and flat for everything
  else. Width-W outcomes widen to the fused length.
- Lanes delimited by an element store fuse into the vector computation
  plus one store, with a `SUM_ROWS` tail where the stored rows are
  reduced.
- A bucket holding more than one template splits at any mid-range writer
  and reconsiders both halves, so an interleaved model still gets the
  lanes on either side of the writer. Splits are capped rather than
  retried to a fixed point.
- One arm rewrites a chain instead of widening it. A lane that multiplies
  two scalars, subtracts a width-m vector, prepends a zero, cumulatively
  sums, and hands the result to a scalar-outcome categorical is one row
  of a `categorical_logit_glm_lpmf`, because
  `cumsum([0, ta - b_1, ...])[j] = j*t*a - sum_{i<=j} b_i`. Buckets refine
  on the subtracted vector (the item), and the slope is whichever of the
  two scalars that refinement holds still.

Fusing is not always a win, so each bucket is costed before it is
emitted, in the currencies the island carver uses: about 5 ns per graph
op against about 1 ns per element moved, with a density element charged
six op dispatches. Two measured pessimizations live in that model rather
than in the shipped graph. Lanes identical down to their immediates and
their external slots are one op once CSE runs, so fusing them re-expands
what CSE would collapse and the bucket is charged for every duplicate it
brings back. And a density with no elementwise kernel trades one
vectorized call for W recorder calls, so those buckets decline until the
kernel gains a form that costs per element what its summed one does
(`bernoulli_logit` and the binomials have one; forcing `multi_occupancy`
past the estimate is a measured 90% regression).

The pass runs after re-rolling and its in-place re-run, so re-roll keeps
first crack at the contiguous shapes it already handles, and before CSE,
which would merge ops shared between lanes and leave no lane whole.
`state_space_stochastic_level_stochastic_seasonal` drops from 1,375 ops
to 19 (2.29x per gradient), `Mth_model` 1,563 to 35 (1.63x), `Mh_model`
1,542 to 18 (1.53x), `Mtbh_model` 1.43x, `Survey_model` 1,427 to 9,
and the two IRT models that reach the GLM arm, `gpcm_latent_reg_irt`
34,634 to 91 (6.5x) and `grsm_latent_reg_irt` (6.3x).

## Common-subexpression elimination (`cse.cpp`, disable: `STANLI_NO_CSE=1`)

The unrolled capture-recapture models evaluate one Bernoulli term per
occasion per individual, and for most of them the arguments are the same
slot: `Mh_model` emits 685 bit-identical `BERNOULLI_LPMF` ops and
`Mb_model` 786, each a full kernel call plus a tape entry on every
leapfrog step. Re-rolling leaves them alone, because they are target
terms with no op consumer, so nothing upstream removes the
recomputation.

This is textbook local value numbering, made safe for a graph whose slots
are mutable buffers rather than single-assignment values. Every write
bumps a per-slot version and a key carries the version of each input, so
two ops separated by a store to something they read are different
computations. Only an op whose output slot is written exactly once in the
whole graph can be the survivor, which is what excludes the destructive
update chains: a slot a later in-place store mutates has two writers.
Effectful and stateful opcodes never merge, and neither does anything
carrying an opaque payload the key cannot compare. Renaming is lazy: ops
are in evaluation order, so resolving each op's inputs through the map as
one forward pass reaches it collapses chains within that pass, where
rescanning the tail after every merge would be quadratic.

Placement is measured, not incidental. Running it before re-rolling
destroys the periodicity re-roll matches on, since the repeated ops it
needs to see are exactly the ones this pass would collapse; running it
after lane partitioning leaves the lanes whole for that pass and hands
the island carver a smaller residue. `Mt_model` falls from 1,062 ops to
70 (16.1x per gradient), `Mh_model` gains 1.39x, and `gpcm_latent_reg_irt`
enters the partition pass with 34,634 ops rather than 61,612. Forward
results are bitwise unchanged.

## Native scalar probability categorical (`message.cpp`)

The active scalar-outcome probability form of `categorical_lpmf` has a narrow
derivative: only the selected probability receives the incoming seed divided
by that probability. Its kernel therefore asks Stan Math's double overload to
compute the value and perform every domain and bounds check, then applies that
single pullback directly. This removes the nested reverse-mode tape from both
sweeps without changing the graph or its operation count.

The contract is intentionally no wider: the outcome must be scalar, the
probability vector must be active, and the call must use probabilities rather
than logits. Array outcomes retain the existing replay so repeated selections
keep the exact log-node and accumulation topology, while categorical-logit
retains replay for its dense softmax pullback. The categorical-logit RBM models
are therefore unchanged controls.

In a targeted 2026-08-24 Release A/B using seven matched-run medians,
`gpcm_latent_reg_irt` moved 1.741465 -> 0.955609 ms/gradient (1.8224x
internally and 1.3998x CmdStan), while `grsm_latent_reg_irt` moved 0.9705208 ->
0.4953192 ms/gradient (1.9594x internally and 1.5387x CmdStan). Time in their
categorical opcode fell 4.45x and 5.00x respectively. These are targeted
medians, not the later full-corpus warmed means of 1.014034 ms (1.32x CmdStan)
and 0.508863 ms (1.50x CmdStan) in `docs/corpus-bench.tsv`.

## Compiled scalar generated-quantities RNGs (`rng.cpp`, `wa_interp.cpp`)

The write-array graph used to stop at every RNG call. Drivers then selected
`WaInterp`, which starts the whole section from statement zero for every saved
draw; even a late scalar draw therefore reinterpreted constrained parameters,
transformed parameters, and all earlier generated quantities.

`OP_RNG` now covers scalar `poisson_log_rng`, `uniform_rng`, `bernoulli_rng`,
`normal_rng`, `lognormal_rng`, and `binomial_rng`. The graph and interpreter
share one draw helper, so they call the same Stan Math function in the same
order on the same caller-owned `WaRng`. An executor holds only a temporary
pointer to that chain resource for one forward sweep; scoped restoration and
copy rebinding prevent pooled or interleaved executors from retaining another
caller's stream. The graph passes treat every RNG op as an ordered effect, so
they cannot fold, reroll, carve, delete, or duplicate a draw whose result is
otherwise unused.

The boundary is deliberately fail-closed. Apart from the audited categorical
and covariance-form multivariate-normal extensions below, container-valued
results or unsupported RNGs still select the whole-section interpreter. Scalar
integer results live exactly in one double slot. The runtime-control tranche
below admits that value only as a checked flat-array/vector index or a branch
input inside a structured register program; loop bounds, shapes, integer
division, and other dynamic-integer operations still refuse lowering.

An exact census of the 24 corpus models that previously used `WaInterp` moved
12 to the graph in this tranche: `GLMM1_model`, both `covid19imperial` models, both `dogs`
models, `hierarchical_gp`, `lotka_volterra`, `one_comp_mm_elim_abs`, `M0_model`,
`Mb_model`, `Rate_4_model`, and `Rate_5_model`. The other 12 remained
interpreted and all 24 retained complete rows. In targeted seven-batch C-ABI
medians, the original eight write-array rows became 34.72x to 80.02x faster.
The two largest absolute wins were `covid19imperial_v2` (156.239 -> 2.089 ms)
and v3 (157.176 -> 2.077 ms). Their C-API construction rises from about 0.239 s
to 2.20-2.21 s because the full graphs are now built, a cost recovered after
roughly 13 output rows.

The four scalar-binomial additions measured 31.93x to 129.55x faster per row:
`M0_model` 15.3778 -> 0.1187 us, `Mb_model` 2020.1042 -> 26.7033 us,
`Rate_4_model` 4.0072 -> 0.1255 us, and `Rate_5_model` 4.3111 -> 0.1238 us.
Across 1,000 rows of each model, their aggregate time falls from 2.0438003 s to
0.0270713 s (75.50x). Construction also improves in all four, so the change
breaks even immediately. Graph and frozen-interpreter output was bitwise exact
for all 28,926 compared values across six seeds and three sequential rows.

## Compiled generated-quantities reductions (`elementwise.cpp`, `lower.cpp`)

Four remaining capture-recapture models stopped at `prod` over a vector or
row-vector and/or `sum` over an integer array assembled from scalar RNG draws.
`OP_PROD_VEC` is a forward-only write-array opcode: it uses Stan Math's Eigen
product grouping while hiding the graph arena's input alignment for
materialized vectors, so an odd slot offset cannot change their packet
boundary. A strided matrix-row operand instead records and preserves Stan
Math's ascending scalar grouping before the expression is materialized.
Lowering admits only a bare materialized vector/row-vector and the exact
outer-subtraction surfaces used by these models. Shifted views, arbitrary
nested expressions, UDF formals, empty containers, and any product that needs
reverse mode remain on the existing fallback. The opcode is explicitly
excluded from reroll matching and island CALLs because it deliberately has no
backward kernel.

Runtime integer `sum` reuses `OP_SUM_VEC` only after lowering proves that the
one-dimensional array is completely initialized in ascending contiguous
writes, every element is an exactly represented integer in a known interval,
and every possible partial sum stays in 32-bit range. Under that proof, the
ascending double additions are exact and equal Stan's integer accumulation.
Uninitialized slots carry CmdStan's `INT_MIN` sentinel on both the graph and
interpreter paths; gaps, strides, unknown RNG ranges, possible overflow, and
using the result as dynamic geometry all fail closed. A proved runtime sum may
subsequently feed the checked index/control surfaces described below, but it is
never treated as a compile-time integer.

This completed `Mh_model`, `Mt_model`, `Mtbh_model`, and `Mth_model`, moving the
then-current 24-model write-array census from 12 graph / 12 interpreter to
16 / 8. The categorical tranche below subsequently advances that census to
17 / 7, the extrema tranche advances it to 18 / 6, the covariance-form
multivariate-normal tranche advances it to 19 / 5, and runtime control below
finishes the current census at 24 / 0. All 119 compiling corpus models still
produce complete rows. Targeted
2026-08-24 C-ABI A/B medians (point 0, two warmups, seven matched batches) were:

| model | interpreted us/row | compiled us/row | speedup |
| --- | ---: | ---: | ---: |
| `Mh_model` | 869.1794 | 12.4941 | 69.57x |
| `Mt_model` | 20.8673 | 0.1266 | 164.83x |
| `Mtbh_model` | 1046.4414 | 9.8556 | 106.18x |
| `Mth_model` | 1098.8496 | 18.2054 | 60.36x |

Across 1,000 rows of each model, aggregate row time fell from 3.035338 s to
0.040682 s (74.61x). Including one construction per model, it fell from
3.056579 s to 0.075491 s (40.49x); the aggregate setup delta amortizes after
five rows per model. Within a 146,196-value comparison, all product-fed RNG
outputs and final sums were bitwise identical to the frozen interpreter,
including a fourth-row stream-continuation check. The two larger models expose a
pre-existing write-array mode-boundary difference in deterministic transformed
parameters: `Mtbh_model` is within two ULP and `Mth_model` within one, while the
compiled graph is closer to live CmdStan at the shared point. These are
targeted medians; the committed full-corpus timing table remains unchanged.

## Compiled categorical generated-quantities RNG (`rng.cpp`, `wa_interp.cpp`)

`categorical_rng` is the first graph-native RNG with a nonscalar argument but a
scalar result. It reuses `OP_RNG`: variants 0 through 5 retain their scalar
double inputs, while the categorical variant reads one variable-length vector
descriptor and writes the one-based Stan int exactly into one double slot. It
stays outside `ScalarRng`, whose arity counts scalar arguments rather than
logical containers.

The lowering gate is deliberately exact: write-array only, one `UVector`
argument, and one `UInt` result. It marks the result initialized but does not
infer the valid `[1, K]` interval, avoiding any expansion of integer range
proofs. `Survey_model` therefore completed its graph first; at that stage
`iohmm_reg` passed the categorical call and still selected `WaInterp` at its
dynamic index. The later checked-index surface below completes it without
changing categorical lowering.

Both execution modes call one helper that copies the already-materialized
probabilities into the exact Eigen vector accepted by pinned Stan Math. Stan
Math therefore remains the single definition of empty-vector and simplex
validation, exception class and message, validation priority, one-based result,
and RNG consumption. Focused tests feed that helper and direct Stan Math the
identical vector and compare sequential draws, rejected-call continuation, and
the next raw engine value. A whole-`Survey_model` graph/interpreter bitwise
comparison is not that oracle: its softmax input already has a pre-existing
deterministic mode-boundary rounding difference.

In a targeted matched C-ABI A/B, `Survey_model` moved from 1011.4736 to
60.3127 us/row (16.7705x), saving 0.9511609 s per 1,000 rows. Construction moved
from 4231.333 to 5406.709 us; the setup delta amortizes after 1.236 rows, or two
whole rows. It was the only change in the exact 24-model census, moving coverage
from 16 graph / 8 interpreter to 17 / 7. All 24 census models and all 119
compiling corpus models retained complete rows.

Across seeds 0, 1, 2, 7, 1234, and `UINT32_MAX`, each continued for three
sequential rows, the categorical draw `n` matched in all 18/18 C-ABI
comparisons. The full rows also contained 8,532 expected bit differences in
deterministic columns from the pre-existing graph/interpreter numerical
boundary; those are not categorical or stream mismatches. These targeted
results do not refresh `docs/corpus-bench.tsv` or the generated full-corpus
table in `docs/benchmarks.md`.

## Compiled generated-quantities extrema (`elementwise.cpp`, `lower.cpp`)

`OP_EXTREMA_VEC` has forward-only variants for `min` and `max`. Its lowering
gate is write-array only: a top-level call must consume a direct `UVector` or
`URowVector` `Var`. The kernel uses the same address-independent grouping that
the pinned Stan Math implementation applies to an owning Eigen value, while
retaining its empty-real results: positive infinity for `min` and negative
infinity for `max`. Arrays, matrices, indexed views, expressions, and UDF calls
remain in `WaInterp`; reverse-mode uses remain refused.

Because the opcode deliberately has no backward kernel, it is explicitly
excluded from reroll matching and interpreter-island CALLs. This keeps the
lowering gate forward-only rather than allowing a graph transformation to
place it in a reverse sweep.

`losscurve_sislob` was the only model to change in the exact 24-model census,
moving coverage from 17 graph / 7 interpreter to the then-current 18 / 6. The
covariance-form multivariate-normal tranche below advances the census to
19 / 5, and runtime control finishes it at 24 / 0. All 24 census models and all
119 compiling corpus models retained
complete rows. Its 1,218-op graph contains exactly one length-10 min opcode and
one length-10 max opcode, and writes 384 columns. Graph and `WaInterp` output
was bitwise exact for all 1,536/1,536 compared values, and the same-input pinned
Stan Math oracle matched 8/8 cases.
Against 1,200 stored CmdStan values, the worst difference was 4.44e-16, or
eight ULP.

In a targeted matched C-ABI A/B, `losscurve_sislob` moved from 329.9520 to
3.3704 us/row (97.8970x), saving 0.3265816 s per 1,000 rows. Construction moved
from 4107.375 to 5291.959 us, a 1184.584 us setup increase that amortizes after
3.627 rows, or four whole rows. These targeted results do not refresh
`docs/corpus-bench.tsv` or the generated full-corpus table in
`docs/benchmarks.md`.

## Compiled covariance-form multivariate-normal RNG (`rng.cpp`, `wa_interp.cpp`)

The vector-result `multi_normal_rng(vector, matrix)` write-array surface reuses
`OP_RNG` rather than adding a second effect vocabulary. Its variant reads a
length-`K` mean and a column-major `K` by `K` covariance, and writes a
length-`K` vector. Lowering admits only an exact non-array `UVector` result,
one non-array `UVector` mean, and one square `UMatrix` covariance whose known
dimensions match. Row-vector/array overloads, mismatched or unknown shapes,
and `multi_normal_cholesky_rng` still select `WaInterp`.

The graph and interpreter call one helper. It reconstructs the same owning
Eigen vector and column-major matrix accepted by pinned Stan Math, invokes
`multi_normal_rng` once, and copies the resulting vector out. Stan Math remains
the single definition of finite-mean, NaN, symmetry, and positive-definiteness
validation order, exception behavior, covariance factorization, output
arithmetic, and engine consumption. Invalid calls therefore consume no draws,
while valid graph and interpreter calls advance the caller-owned stream
identically. The existing opcode-wide effect barriers keep the vector draw out
of constant folding, rerolling, and islands without another pass-specific
exception.

`multi_occupancy` was the only model to change in the exact 24-model census,
moving coverage from 18 graph / 6 interpreter to the then-current 19 / 5. The
runtime-control tranche below subsequently completes those five, for 24 / 0
and graph write arrays in all 119 compiling corpus models. Graph and
forced-interpreter output was bitwise exact for all 5,616 values across seeds
0, 1, 2, 7, 1234, and `UINT32_MAX`, each continued for three sequential rows.

In a targeted 2026-08-25 matched C-ABI A/B (point 0, two warmups, seven batch
medians), `multi_occupancy` moved from 298.9260 to 5.4898 us/row (54.4512x),
saving 0.2934362 s per 1,000 rows. Construction also improved from 5864.792 to
5368.583 us, so there is no setup break-even penalty. These targeted results
do not refresh `docs/corpus-bench.tsv` or the generated full-corpus table.

## Compiled generated-quantities runtime control (`mir_prog.hpp`, `lower.cpp`)

The final five interpreted write arrays were four Viterbi decoders and
`iohmm_reg`. Their shapes and loop bounds are compile-time values, but their
branches and backtracking indices are known only for the current draw. Lowering
now translates an enclosing generated-quantities block into one structured
`IslandProg`: compile-time loops are still unrolled, while comparisons and
jumps run inside the register program. Multidimensional scalar arrays retain
their declared extents and CmdStan's `INT_MIN` default for integer cells. A
runtime backpointer read uses the checked `Program::DYN_INDEX` instruction, and
`max` over the final state row delegates to pinned Stan Math over an owning
vector.

`OP_DYNAMIC_SLICE` covers the graph-side companion operation: one 1-based
runtime integer selects a fixed-width element from a vector or from one outer
array dimension. Its backward scatters only to the selected block and rereads
the protected index; malformed geometry and nonintegral/out-of-range indices
throw. Shapes, loop bounds, matrix rows, nested outer dimensions, and other
dynamic geometry remain outside this surface.

An op has six input descriptors, while the largest Viterbi block reads more
logical values. Lowering packs only the excess leading live-ins with
`OP_CONCAT2`; each program live-in records its descriptor and offset. Both the
generated adjoint and var replay scatter through the same map. A direct test
pins values and all seven gradients in both modes, and the generated-quantities
fixture forces eight logical live-ins through six descriptors.

This moves `hmm_drive_0`, `hmm_drive_1`, `hmm_example`, `hmm_gaussian`, and
`iohmm_reg` from the interpreter to the graph. The exact 24-model historical
census is now 24 graph / 0 interpreter, and the full write-array census is 119
of 119 graph-backed, with every row complete. Targeted 2026-08-25 C-ABI A/B
medians (point 0, two warmups, seven batches) were:

| model | interpreted us/row | compiled us/row | speedup |
| --- | ---: | ---: | ---: |
| `hmm_drive_0` | 6776.8490 | 41.9750 | 161.45x |
| `hmm_drive_1` | 7396.1172 | 44.4255 | 166.48x |
| `hmm_example` | 1388.2539 | 9.7183 | 142.85x |
| `hmm_gaussian` | 37365.6562 | 649.1253 | 57.56x |
| `iohmm_reg` | 38642.0417 | 713.0111 | 54.20x |

Across 1,000 rows of each model, aggregate row time falls from 91.568918 s to
1.458255 s (62.79x). Including one construction of each, it falls from
91.989657 s to 1.990729 s (46.21x); the aggregate setup increase amortizes
after 1.240 equal-mix cycles, or two whole rows per model.

The two drive models and `hmm_example` matched the forced interpreter bitwise
for every column at four test points. Across four points, three seeds, and
three sequential rows, all 51,696 checked Viterbi outputs in the four HMMs
matched bitwise. For `iohmm_reg`, all categorical states and Viterbi
states/scores matched; 2,945 continuous simulated observations inherited the
pre-existing graph/interpreter transformed-input difference, bounded by
8.89e-16. Exact later state draws prove the shared RNG stream remained aligned.
These targeted medians do not refresh `docs/corpus-bench.tsv` or the generated
full-corpus table.

## Control flow that depends on a parameter (`lower.cpp`, `mir_prog.hpp`)

`if (theta > 0) ...` cannot become ops: the op list is fixed at load
time, and this statement picks its branch anew at every evaluation. The
region (just the conditional, not the model around it) is translated
into the same instruction list the ODE right-hand sides use, and one op
runs it: on plain numbers for the value, and again under stan-math's
own autodiff for the derivatives, which are the derivatives of
whichever branch ran. That is what CmdStan's generated C++ does for the
same statement. This is not an optimization and no switch turns it off;
without it the model does not run.

One refusal: `y ~ normal(mu, 1)` inside such a region is rejected with
a message saying to write `target += normal_lpdf(y | mu, 1)` instead.
The `~` form drops terms based on which arguments are parameters, a
distinction the instruction list cannot make because it binds every
value the same way. Getting it wrong would leave the gradient perfect
and the log density off by a constant, so it is refused rather than
approximated.

Because the region has to translate or the model does not compile at
all, anything the instruction list cannot say is a hard error rather
than a slow path -- which is why its density vocabulary is the runtime's
whole list and not a subset of it. It used to be twelve hand-picked
ones, and `target += chi_square_lpdf(y | nu)` inside an `if` on a
parameter did not compile while the identical line outside the `if`
did. One list now
([`program_density.hpp`](../include/stanli/program_density.hpp)), shared
with the MIR interpreter and with the generated derivatives, so a
density added to the runtime arrives in all three at once.

## Tape islands (`island.cpp`, disable: `STANLI_NO_ISLAND=1`)

Some code cannot be vectorized by anyone: an HMM's forward recursion
computes step t from step t-1, and every step depends on parameters.
Re-rolling correctly refuses these. This pass runs last, so what it
sees is by construction what nothing else could help. It compiles each
long stretch of leftover scalar ops into a single instruction list run
by one op, forward on plain numbers.

The backward used to re-execute that same list under stan-math's
autodiff on a scratch tape and read the derivatives out. That gave the
right answer by construction -- the same arithmetic CmdStan performs for
those statements -- and it cost what CmdStan costs, which was the whole
problem. Collapsing thousands of ops into one sounds like it must be
faster, and with that backward it mostly was not: measured on all
fourteen corpus models with a big enough region, it was faster on one, a
wash on four, and up to 20x slower on nine. The op count was never the
cost; scalar ops are about as cheap as the arithmetic inside them, and
replaying under autodiff is not.

So the backward is *generated* instead (`adjoint.cpp`): the compiler
reads the instruction list once and writes a second instruction list
that computes the derivatives directly, in plain numbers, allocating
nothing. Each rule is the matching stan-math derivative transcribed
expression for expression, so the two still agree to the last bit on
almost every region, and `STANLI_NO_NATIVE_ADJ=1` switches back to the
autodiff replay to check that they do.

That changed the class rather than improving it. On the corpus
models with a region big enough to compile, every one is faster
generated than replayed, and models that were a wash became real wins:
one HMM collapses 42,926 ops into 11 and went from 0.92x to 1.60x. The
model whose steps copy a 1,500-element state vector -- memory traffic
the register file makes free, which is why it was the one winner before
-- went from 2.5x to 4.7x.

The pass still estimates both sides before committing, because one shape
is still wrong for it: a region carrying far more state than it
computes, where the register file costs more to write and read back than
the ops ever moved. Value and adjoint storage are accounted separately.
Every value or checkpoint register other than CALL scratch is charged for
its forward write and backward read. Checkpoints have no adjoint cell, and
copies that share a stan-math vari share an adjoint equivalence class; those
classes are packed densely and charged once for the runtime's zeroing pass.
The estimate then adds both instruction lists, the graph's per-op dispatch
cost, and a cost-neutral correction for calls to graph kernels. This is the
same layout the native island backward allocates and clears, rather than a
fixed weight per forward register.

On `iohmm_reg`, 95,424 forward register ids contain only 39,000 distinct
adjoint classes, while 4,488 checkpoint registers are value-only. Correct
accounting moves the estimate from 389,640 to 328,728 against a graph cost of
361,045, so the default path now takes the same 27-op island as
`STANLI_ISLAND_ALWAYS=1`. A targeted seven-run Release A/B measured
498.612 -> 241.453 us/gradient (2.065x). A default/disabled/forced census of
all 21 corpus models with compilable regions changed only this decision and
preserved every known refusal, including `bones_model`, `dugongs_model`,
`Survey_model`, and both `covid19imperial` models. The environment switch
still skips the estimate, which is how to ask why a region was left alone.

Vocabulary is no longer a refusal for scalar work. The machine has its
own instructions for the arithmetic a recurrence is made of, and
anything else with a single-number result -- a cdf, a discrete density,
a special function -- compiles as a *call to the graph's own kernel*,
the exact code the op would have run, partials and derivative included.
So one op the machine has no instruction for stops ending a run, which
used to split regions in half; a call buys continuity, never speed, and
the estimate charges it accordingly. Vector-result ops still end runs.

The pass still refuses outright: short runs (under 32 ops), regions with
more than six distinct inputs, densities in the dropped-constants form
(see the refusal above), and regions producing target entries. One more
applies to the generated backward alone: a region with a branch on a
parameter keeps the autodiff replay, because reversing a branch needs
the nested if/else shape the flat instruction list has already thrown
away.

### Register-program compaction (`program.cpp`, disable: `STANLI_NO_ISLAND_COMPACT=1`)

A region's instruction list arrives full of the MIR's declaration
bookkeeping: a fill of the language-level default, then a copy of the real
initializer, then more copies through return temporaries. Across the ten
corpus models with a compiled region, 39% of the instructions were copies.
The same pass that removed them from ODE right-hand sides
(`compact_program`) now runs over islands too, before the adjoint generator
reads the forward -- so the backward is generated from the compacted
program rather than remapped onto it -- and it drops the registers that
leaves unreferenced, which shrinks the value file the forward writes and
the backward reads.

The copy test is deliberately the same one `gen_adjoint` applies before
letting two registers share an adjoint cell, and a copy this pass will not
remove keeps the fill that made `gen_adjoint` refuse it. Both halves of
that are what keeps the generated backward's arithmetic identical: the
whole corpus is byte-for-byte unchanged with the switch on and off.

Measured (Release, median of nine interleaved runs): `iohmm_reg` 40,968 ->
31,968 forward instructions and 56,501 -> 43,488 registers, 1.040x;
`hmm_example` 1.057x; `hmm_gaussian` 1.025x; `hmm_drive_0` 1.015x. `garch11`
and both `accel_*` models are unchanged, their copies being the ones the
rule declines.

What is left for this class is the per-instruction cost itself. Both
directions read one instruction at a time and decide what to do with it,
where CmdStan's compiler has inlined the equivalent straight into the
model's machine code.

## Native symmetric eigendecomposition pullbacks (`matrix_fns.cpp`)

Stan Math's reverse overloads for `eigenvalues_sym` and `eigenvectors_sym`
retain a full symmetric eigendecomposition on their autodiff tape. The old
kernels ran the primitive double operation in the forward sweep, discarded
that decomposition, then rebuilt the input on a nested tape and solved it
again during the backward sweep.

The forward kernels now retain exactly the missing half of the solver result
in scratch: eigenvectors beside an eigenvalue output, and eigenvalues beside
an eigenvector output. Their backwards transcribe Stan Math's matrix
pullbacks expression for expression, using the retained decomposition and
plain doubles. `forward_value_only()` keeps the original eigenvalues-only
solver and writes no backward scratch, so initialization does not pay for
work it will not use; a later gradient always refreshes the scratch with a
normal forward first.

On `kronecker_gp`, two matrices each feed both symmetric-eigen operations.
This removes four reverse-time eigensolves per gradient and moves median
latency from 289.0 to 185.7 us (1.56x internally, 1.17x CmdStan). The two
forward operations on each matrix remain separate; sharing them would need a
paired graph operation and is deliberately outside this kernel-only change.

## Native single-vector Cholesky normal (`matrix_fns.cpp`)

The `gp_regr` likelihood is one `multi_normal_cholesky_lpdf` observation with
data `y` and `mu`, an active Cholesky factor, and `propto=true`. Stan Math has
an analytic matrix partial for that exact instantiation, but the generic
kernel rebuilt an elementwise var matrix and nested tape in both sweeps.

The exact `variant=0x84`, one-observation shape now evaluates Stan Math's
triangular inverse, half-vector, value, and full factor-partial matrix in the
same expression order during forward, retaining the partial matrix in
scratch. Backward is only the seeded accumulation into the factor. All other
activity masks, vectorized observations, and non-propto calls keep the generic
Stan Math replay. On `gp_regr` the density falls from 2.48 to 0.70 us and the
whole gradient from 6.05 to 4.20 us (1.44x internally, 1.12x CmdStan).

## Compiled ODE right-hand sides (`ode_prog.cpp`, report fallbacks: `STANLI_DEBUG_ODE=1`)

An ODE model passes a user function (the right-hand side) to a solver
that decides at run time when to call it, so this one function cannot
be unrolled into the graph. It used to be run by walking the compiler's
syntax tree per call, with a map lookup per variable and an allocation
per intermediate: about 6 us per call, hundreds of calls per gradient,
97% of the total time on the ODE models.

Now the function is translated once, at load, into a flat instruction
list: variables become numbered cells, known-bound loops unroll,
run-time conditions become jumps, and a call is one loop over the
instructions with no lookups and no allocation. Anything the translator
cannot handle falls back to the old interpreter (slower, never less
supported); `STANLI_DEBUG_ODE=1` prints when that happens. A test runs
every supported construct through both implementations and requires
bit-identical results.

Separately, the gradient used to solve the ODE system twice, once for
values and once for derivatives. The first solve already computes
everything the derivatives need, so it is kept instead of thrown away
and the second solve is gone.

The ODE opcode also records the scalar types chosen by stanc separately for
the initial state and parameters. This is not inferred from executor adjoint
storage: a value can have an adjoint buffer without being instantiated as a
Stan `var`. The solver therefore integrates only the sensitivity systems for
inputs that are actually autodiff types, while retaining a fixed Jacobian
scratch layout and gating inactive columns in the backward sweep. On
`one_comp_mm_elim_abs`, whose initial state is data, this reduces the
sensitivity width from four to three and median latency from 699 to 639 us
per gradient (1.09x); fully active ODE models are unchanged.

The compiled right-hand side also used to allocate promoted `y` and `theta`
staging vectors on every callback. It now seeds those inputs directly into the
reusable result-scalar register file in the original promotion order, including
unused parameter source slots, before executing the unchanged instruction
list. Solver math, output ownership, and the interpreter fallback are
unchanged. A targeted 2026-08-24 Release A/B (seven alternating matched-batch
medians) measured:

| model | staged inputs | direct register seeding | improvement |
| --- | ---: | ---: | ---: |
| `lotka_volterra` | 78.6216 us | 68.6068 us | 1.14597x |
| `soil_incubation` | 102.1527 us | 89.5593 us | 1.14062x |
| `one_comp_mm_elim_abs` | 643.1571 us | 578.8621 us | 1.11107x |

The geometric-mean improvement is 1.13245x, or 38.5-39.3 ns saved per callback
at the known callback counts. Three evaluation points produced bitwise-
identical LP and gradient values for all 63/63 checked scalars. This dated
targeted A/B isolates the direct-seeding change; the later full-corpus warmed
means and CmdStan ratios remain the absolute source of truth in
`docs/benchmarks.md`.

The original RHS compiler, solve reuse, and mixed-activity work made the ODE
models 29-39x faster; direct input seeding compounds that historical gain.

The remaining Jacobian extraction also used more reverse-mode machinery than
the ODE result contains. Stan Math returns each solution scalar as one
`precomputed_gradients_vari` directly connected to the active inputs. Calling
`grad` once per output therefore walked every sibling output node merely to
chain the selected node. The kernel now zeros the nested adjoints and chains
that selected result node directly. Between rows it clears only that output
and the active inputs it touched; the graph backward still consumes rows
last-to-first and accumulates inputs in their original order.

A narrow load-time cleanup complements that change inside the callback. MIR
spells initialized scalar locals as a default constant followed by the real
assignment and copies values through return temporaries. For an RHS containing
only scalar, effect-free instructions, the compiler removes an initializer
overwritten before any read/control-flow edge and aliases a `MOV` only when
both registers have stable single definitions. It remaps jump targets after
compaction. Any dynamic indexing, range read or transform, density, kernel
call, or other unfamiliar instruction retains the original program unchanged.

A targeted 2026-08-25 Release A/B used three isolated binaries from the same
parent, seven rotating matched batches per model, a 200 ms warmup, and roughly
one measured second per batch:

| model | parent | direct Jacobian node only | node + RHS cleanup | total improvement |
| --- | ---: | ---: | ---: | ---: |
| `lotka_volterra` | 78.5160 us | 70.3923 us | 60.8881 us | 1.28951x |
| `soil_incubation` | 101.3979 us | 88.5437 us | 82.0553 us | 1.23573x |
| `one_comp_mm_elim_abs` | 663.9302 us | 665.3790 us | 629.7752 us | 1.05423x |

The full change improves the geometric mean by 1.18876x. The same fresh
parent and patched checkers produced byte-identical LP and gradient output at
three evaluation points for all three models (63/63 scalars). This is a
targeted mechanism A/B; it does not replace the full-corpus warmed means or
CmdStan columns in `docs/corpus-bench.tsv`.

## Executor details (`executor.cpp`)

- Each op's context (the pointers telling the kernel where its inputs,
  outputs, and scratch are) is built once at setup; nothing moves
  afterwards, so there is nothing to rebuild per evaluation.
- All values live in one arena. Adjoint storage is a separate compact
  layout: parameters first, then only slots surviving ops write (plus a
  constant result). Data and slots left behind by graph rewrites therefore
  consume no adjoint memory and are absent from the one reset `memset`.
  Both arenas are allocated once; a steady-state gradient evaluation
  performs no allocation.
- Kernel function pointers are resolved once at setup into two flat
  lists (forward order and reverse order, with backward-less ops left
  out of the second), and both sweeps are unrolled four at a time.

On `iohmm_reg` this packs 1,604,979 adjoint doubles down to 3,084 and
moves the median gradient from 282.9 to 233.7 us (1.21x). Values deliberately
keep their original slot layout: a compiler-final value tombstone prototype
was correct but shifted hot buffers and consistently slowed `Mtbh_model` by
about 8%, so it is not part of this optimization.

A tail-call threaded dispatch, the usual next step, measured slower
than the unrolled loop (`tools/bench_dispatch.cpp` keeps the
comparison). After these changes per-op cost is dominated by loading
the context, not the dispatch, so the remaining lever is fewer ops,
which is what the passes above are.

## Deferred: reduction reassociation (surveyed 2026-08-25)

The candidates below change summation order rather than any elementwise
result. Each was measured or profiled during the 2026-08-25 precision survey
and deferred as a policy choice: the corpus reference gate passes either way,
but models listed as bitwise against CmdStan in
[`docs/corpus-status.md`](../../docs/corpus-status.md) would move into the
small-ULP bands. Re-profile before implementing any of them, since profile
share is a ceiling on the win and the `pow` entry below shows how a large
share can still yield nothing.

Softmax backward as a packet dot product (`adjoint.cpp`). The fold is written
by hand because Stan Math reduces var expressions, which have no packet
access. `Map(p).dot(Map(oa))` measured 5.86x on the reduction alone.
`OP_SOFTMAX` is 37% of `gpcm_latent_reg_irt`'s gradient and most of that is
backward. Drift is reorder-class, about 1e-15 relative on realistic lengths.

Matvec through Eigen gemv (`elementwise.cpp`, `lower.cpp`). The scalar loop
preserves Stan Math's multiply order at a cost the file comment puts at 82% of
`prophet`'s gradient. Every matrix model pays something. Drift is 1-2 ULP per
element against the current form.

Gather backward via segmented reduction (`elementwise.cpp`). The ascending
scatter-add matches var edge order; a rowwise sum over a presorted index
(indices are lowering-time constants) was 37% of
`radon_county_intercept`'s backward.

Per-lane scalar density binding (`densities_impl.hpp`). N scalar recorder
calls with a `sink_scope` each, where one vectorized call would do. Density
share is 40-86% of most profiles but the math is libm-bound either way; the
recoverable part is recorder overhead plus reduction reorder and was not
measured separately.

Broadcast adjoint accumulation (`eltwise_expr.cpp`, `mixture.cpp` `mix_bwd`,
`densities_impl.hpp` column sums). Descending or in-memory accumulation orders
that mirror the unrolled reverse sweep. Packet sums are 4-6x on the reduction;
the whole-model effect is small per site but the pattern is wide.

Three measured dead ends, recorded so they are not retried. `-ffp-contract=fast`
is a 1.7% geomean slowdown on this corpus (`arK` 0.85, reproducible) and
perturbs 101 of 120 models at reduction-class magnitudes, so the `off` in
`CMakeLists.txt` stays. The `STANLI_PACKET_MATH` fast forms (constrain,
`inv_logit`, `seq_sum`) measure 0.995 geomean: neutral, correctly left off.
And Eigen's vectorized `pow` is 5x slower than scalar `std::pow` on Apple
libm, with the `exp(k log x)` form costing 8 ULP, so the `POW` share of the
`dogs` models stays where it is.

## How we check all of this

Every pass changes the graph, and a wrong graph produces wrong numbers
silently. Three layers:

- Unit tests per pass (`tests/test_reroll.cpp`, `test_inplace.cpp`,
  `test_island.cpp`, `test_ode_prog.cpp`, `test_pass_safety.cpp`),
  including tests that the passes refuse what they must refuse, and a
  fuzz test that runs hundreds of random graphs through all passes and
  compares gradients before and after.
- A full-corpus A/B check (`harnesses/ab_corpus.py`): every posteriordb
  model evaluated with the passes off and on, comparing the log density
  and every gradient component. The passes-off graph is separately
  verified against CmdStan, so if A and B agree, the optimized graph
  inherits that verification. This has caught two real bugs the unit
  tests missed. Run it after any change to a pass.
- Direct verification against CmdStan (`tools/verify_sample.py`) for
  models a change affects.

The env switches (`STANLI_NO_DATA_PRELOAD`, `STANLI_NO_INPLACE`,
`STANLI_NO_CONSTFOLD`, `STANLI_NO_REROLL`, `STANLI_NO_ISLAND`,
`STANLI_NO_ISLAND_COMPACT`) exist so a wrong
result can be attributed to one optimization quickly, and they are how each
one is measured: every speed number is the same build with one variable set
and unset.
