# Graph optimizations and performance work

What stanli does to a model's op graph before running it, written for
people who have not worked on compilers. Each section says what the
problem is, what the pass does, and how to turn it off when debugging.
Targeted measurements are included where available. Current absolute results
across the model corpus are in
[`docs/benchmarks.md`](../../docs/benchmarks.md).

The switches in this file are developer switches. They work only in a build
configured with `-DSTANLI_DEBUG_SWITCHES=ON`, which the wheels, the R runtime
and the browser builds do not use
([`docs/hacking.md`](../../docs/hacking.md#building)). A switch that is set in
another build is ignored, with a one-time notice. [Developer
switches](#developer-switches) lists every switch, what it does and the file
that reads it. Corpus sizes and model counts in the measurements are the ones
at the time of each measurement. The corpus has grown since the early ones;
[`TESTING.md`](../../TESTING.md#every-model) has the current count. The file names
below that have no directory are in
`runtime/src/`, except the kernels, which are in `runtime/kernels/`, and
headers such as `mir_prog.hpp`, which are in `runtime/include/stanli/`.

## Data input preload (`lower.cpp`, disable: `STANLI_NO_DATA_PRELOAD=1`)

stanc's transformed MIR declares each input and reads it with one
`FnReadData` statement. That is useful to generated C++, but redundant here:
`DataMap` has already parsed the JSON into the same typed, column-major
layout. Older stanc releases copied each container element by element from a
flat buffer, which was especially expensive for matrices.

Lowering therefore binds declared inputs directly and skips those
declarations and `FnReadData` statements. Every other statement still goes
through the transformed-data interpreter. User-written transformed-data
statements always run; `STANLI_NO_DATA_PRELOAD=1` remains the exact interpreter
oracle for auditing future stanc output changes.

On `nn_rbm1bJ100`, whose MNIST input is a 60,000 x 784 matrix, this reduces the
`bind_data` stage from 23.56 s to 0.09 s and complete graph compilation from
23.80 s to 0.23 s. It also avoids carrying an integer mirror merely because
the JSON happened to spell a real matrix with integer tokens. Set
`STANLI_NO_DATA_PRELOAD=1` to replay the generated hydration as a correctness
oracle.

## Loop-invariant target collapse (`lower_stmt.cpp`)

Before unrolling a `for` loop, lowering checks whether its only externally
visible effect is adding iterator-independent terms to `target`. Such a loop
is lowered once and each target term is multiplied by its data-computed trip
count. Nested invariant loops accumulate their counts before the target edge,
so `N` repetitions of `M` repetitions emit one scale by `N*M`, not two
multiplications or `N*M` density ops.

The proof is deliberately syntactic and fail-closed. Every expression in the
body must be independent of the iterator, assignments may name only locals
declared under the loop, and the statement whitelist admits only local
declarations/assignments, target increments, blocks, `for`, `if`, and `skip`.
Checks, print/reject, RNGs, `target()`, ODE/solver/callback calls, UDFs, loop
control, and unknown statement or expression kinds retain the ordinary
per-iteration path. A zero-trip loop does not lower its body; a one-trip loop
uses the old path unchanged.

On the issue #248 reproducers, both `N = 10,000,000` and nested
`N = 4,000, M = 1,440` now finish log-prob lowering with two ops (one density,
one multiply). On an M3, the former's log-prob lowering takes about 0.05 ms and
the full log-prob preparation about 0.09 ms, with the check process at about
4.7 MB maximum RSS. The previous unroll took 34.8 s and 4.38 GB at ten million
iterations; the nested case took 19.2 s and 2.0 GB.

As with re-rolling, this intentionally changes last-bit numerics: multiplying
one density value rounds once instead of adding identical values through the
six-way target tree. The mathematical result and gradient are the same, and
the corpus oracle remains inside its existing per-model gates.

## Background: why op count matters

stanli turns a Stan model into a list of operations ("ops"). Each op
reads some buffers, computes something, and writes a buffer. The log
density is the list run top to bottom; the gradient is the list run
bottom to top.

Running one op costs a fixed overhead regardless of how small the
computation inside it is. The cost models of the passes below
(`pass_util.hpp`, `island.cpp`) put it at about 5 nanoseconds per op. An op that adds two numbers pays the same
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
without changing the model's results beyond last-bit rounding. They run in this
order, from `Lowering::run_passes` in `lower.cpp`:

1. splitting of target sums (`target_sums.cpp`, log density only)
2. folding of diagonal updates into GP covariances (`gp_cov_fusion.cpp`)
3. in-place updates (`inplace.cpp`)
4. store-to-load forwarding (`inplace.cpp`)
5. constant folding (`constfold.cpp`)
6. loop re-rolling (`reroll.cpp`)
7. in-place updates again, for slice stores created by re-rolling
8. observation collapse (`collapse.cpp`, fast mode only, not in `write_array`)
9. lane partitioning (`partition.cpp`)
10. in-place updates again, for slice stores created by partitioning
11. eliding of full-extent stores (`inplace.cpp`)
12. common-subexpression elimination (`cse.cpp`)
13. tape islands (`island.cpp`)

Each stage has a section below. Not every stage runs on every
graph. A `PassPlan` in `lower_internal.hpp` switches five of them on or off:
constant folding, partitioning, eliding stores, CSE and islands. The
log-density graph runs all five. The `write_array` graph is forward only and
has no target to reduce, so it runs constant folding and CSE and skips the
other three. The body of a higher-order callback (`kCallbackPasses`) runs
partitioning, eliding and CSE. It skips constant folding, because its inactive
inputs are bound at run time and folding would evaluate them against zero
storage, and it skips islands. The two in-place re-runs do work only if the
stage before them replaced something.

Fast mode (`CompileOptions::fast_math`) does change the numbers, within the
fast-mode gate, and it touches four of these stages. It adds observation
collapse. It lets re-rolling and partitioning fuse lanes that read one active
scalar at several positions (`fuse_shared_params`); in the default mode they
leave such a region scalar, because widening it would regroup that scalar's
adjoint contributions. Re-rolling then also prices each position by its own
number of distinct lanes. And CSE merges duplicates of an active value
completely (`merge_active`), where the default mode shares the value and keeps
each pullback. The sections on [re-rolling](#loop-re-rolling-rerollcpp-disable-stanli_no_reroll1),
[partitioning](#lane-partitioning-partitioncpp-disable-stanli_no_partition1) and
[CSE](#common-subexpression-elimination-csecpp-disable-stanli_no_cse1) say
where each of these applies.

## Splitting target sums (`target_sums.cpp`, no switch)

A model can add terms to a local, `lp += f(n);` in a loop, and add the local to
`target` once. A `reduce_sum` partial-sum function does this. The target term
is then one long chain of scalar additions, which re-rolling cannot treat as
separate per-element terms. The pass replaces such a chain with its operands as
separate target terms, so that re-rolling and partitioning can vectorize them.
The target is already a sum in an order that stanli chooses (`reduce_terms`
adds the terms in groups), so splitting a chain only picks a different order
of the same terms. It runs first, and only on the log-density graph, so that every pass
configuration sees the same terms.

An addition is removed only when it is the sole writer of its result and its
one reader is the next addition in the chain or the target. A chain with a
repeated operand, or one shorter than the re-roll lane threshold (four), is
left alone, and a `+0.0` operand is dropped.

## Folding diagonal updates into GP covariances (`gp_cov_fusion.cpp`, no switch)

A covariance function followed by a loop that adds one scalar (a jitter) to
each diagonal element lowers to the covariance op and then, for every diagonal
element, an index read, an add and an indexed store. The pass folds a complete
chain of these into the covariance op, which takes the scalar as an extra
input. It applies to the Matern 3/2, Matern 5/2, exponential and periodic
covariances. Some of these return diagonal variables that alias one another in
Stan Math, so materializing the matrix between the update ops would lose their
reverse accumulation order. The exponentiated quadratic covariance has an
upstream matrix callback with distinct output nodes, and the pass leaves it
alone. Fusing keeps the covariance and its diagonal additions on one Stan Math
tape. The pass refuses an incomplete chain, and a chain whose intermediate
values anything else reads.

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
this, the writes are deleted too. When a write is followed by another
write to the same element with no other read of the vector in between
(`p[i] = a + b; p[i] = inv_logit(p[i]);`), the first write is deleted as
well. This turns "loop that fills a vector" into "loop that does
arithmetic", which re-rolling knows how to vectorize.

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

Unlike the other `STANLI_NO_*` developer switches, which act when they are set
to any value, `STANLI_NO_CONSTFOLD` acts only when its value starts with `1`.

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
- Reading a row or column at an offset that follows the data, like
  `z[, subject[n]]`: one gather op over every lane's window. Nothing may
  write the base inside the loop, so every lane reads what its scalar op
  would have read.
- Writing `v[0], v[1], ...` across lanes: one store op, or no op when
  the writes cover the whole vector (the computed vector simply becomes
  `v`). Writes stepping by a fixed stride (filling a matrix column by
  column) use a strided store. When several write runs take turns
  filling one vector, each run's store output becomes the vector
  everything later refers to, so the runs chain and each vectorizes on
  its own.
- A run of writes that all write the same value: keep the computation
  scalar and broadcast it across the range.
- Read-modify-write (`mu[n] += ...`): a lane may read the element it is
  about to overwrite, and only that element. Lane targets are distinct, so
  each read sees the vector as it was before the run, and the read becomes
  the whole vector, a slice or a gather of it. A lane that reads another
  element (`mu[n] = mu[n - 1] + ...`) is a recurrence and stays scalar.
  Rows of a matrix follow the same rule.

A dot product of two row-wide operands, like `X[n] * (beta + z[, s[n]])`,
becomes one `OP_GROUP_DOT`. Constant folding leaves each data row `X[n]` as
its own constant, and those constants pack into one data matrix like the
per-lane scalar constants above. The lanes pack row-major, so each lane's
two rows are one contiguous group, and the group dot sums each group with
the same Eigen expression as `OP_DOT` (variant `kGroupDotAsDot`). Each
lane's value is therefore bitwise the scalar one. The gradient adds each
lane's contribution to a shared vector such as `beta`, or to a gathered
element of `z`, in a different order than the scalar lanes did. On issue
657's model (N = 5000) the folded loop drops from 20,014 log-density ops
to 25 and keeps the same accuracy against an 80-digit reference.

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

A lane may also be C elements wide (`LaneLayout`, the row-major/column-major
convention one region's wide row ops must agree on). A row read covering
its base elides to it the same way a scalar `OP_INDEX` does; a partial row
or a per-lane gather packs through one `OP_SLICE` or `OP_GATHER` once its
lanes' offsets are, together, one affine run (the same classifier
`builtin_index_map` uses for a static selector, extracted so there is one
place that decides what an affine window is); an invariant operand as wide
as the row tiles via `OP_REP_MAT` instead of blocking the region; and a row
density whose result feeds another op (a mixture over rows) folds back with
one `OP_SUM_ROWS` per lane, repacking first when the region's convention is
column-major, since `OP_SUM_ROWS` needs each lane's elements contiguous.
A lane that builds its row with `OP_CONCAT2` (`mu[n] = [a[n], b[n]]'`, a shape
that brms generates for categorical and multivariate models) commits the
region to column-major, where the lanes' concatenations are one `OP_CONCAT2`
of the packed operands; a shared operand, such as a leading zero, tiles to
every lane. A row store
whose container uses the other convention takes one `OP_TRANSPOSE`. When
that transpose's input is the widened chain of C length-L columns, the
chain would copy its growing prefix once per link, so instead the first
column fills the row-major matrix (`OP_REP_MAT`; a scalar such as the
leading zero fills it whole) and each further column goes in with one
`OP_SET_SLICE_STRIDED_INPLACE` of stride C. Each element is copied once
and the transpose disappears. Row-major row stores may fill any run of consecutive rows through one
`OP_SET_SLICE` window. Store forwarding often leaves the last row out of
the run, because that lane reads its values directly (#574).
Partition's `indexed_read` and its own wide-operand check share the same
affine classifier and the same tiling, so a lane found by structure there
or by period here is priced and packed the same way.

Fixed-width row reductions get one deliberately narrower pre-pass. It
recognizes a short vector that an outer loop fills element by element and then
reduces with `log_sum_exp` into the target once per iteration (the topic-model
likelihood has this form, with a vector of K entries per document). A width
below the ordinary lane threshold (`kMinLanes = 4`) cannot be re-rolled at all,
and at larger widths the scalar reduction between rows stops the outer loop
from looking periodic. So the exact concrete grammar is recognized as a unit:
two indexed reads, logs, an add, the complete element stores, and a
target-only reduction. Consecutive rows
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
tries again on the rest, which handles data that comes in blocks. A lane
count stops where an element store's index stops marching (a column-major
matrix filled row by row fuses one row at a time), because counting and
classifying the rest of the run again at every block made preparation
quadratic in the rows. The reasons a run stays scalar are reported as
described under
[why a group of lanes stays unfused](#why-a-group-of-lanes-stays-unfused).

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

The periods tried at a position come from the graph. The first candidate op
in the window must recur one period later, so the candidates are the distances
to its next ops with the same signature: every one up to 32 ops away, then the
next eight. Earlier
versions tried every period up to 32 ops, which left a brms partial sum with 11
or more varying-effect terms (35 ops per iteration) entirely scalar: 7.9 ms per
gradient where 10 terms took 0.46 ms (issue 571).

A classified region still prices itself before committing, in the same
currencies `partition.cpp` uses (`pass_util.hpp`'s `kLaneOpCost`,
`kLaneDensityElem`, `kLanePartitionMargin`): elision and hoisting free, a
slice or gather at their element count, a store only when it is not the
whole-vector or write-chained form, and a CSE-collapse charge
(`(Luse - distinct) * lane_elems`) for lanes distinguishable only by an
immediate CSE would otherwise merge. In fast mode, where CSE also merges
active duplicates, the charge is made for each position by its own count of
distinct lanes. A region that fails to clear its own
cost declines through the same shrink-and-retry path a hard classification
failure already takes. In the default mode a region also declines when one
active scalar is read at more than one of its positions: widening evaluates one
reverse operation across all lanes before the next, so that scalar would
receive its contributions regrouped. Fast mode lifts this refusal
(`fuse_shared_params`).

This matters most exactly where it looks least intuitive: a discrete
idata-density region whose lanes carry mostly repeated outcomes (a
capture-recapture model where most subjects share one of a handful of
encounter histories) prices as `distinct` near 1 against `Luse` in the
hundreds, so the vector form's per-element cost dominates the comparison and
the region declines. Measured, not assumed: on `M0_model` and `Mt_model`
(posteriordb), packing the shared-history subjects into one wide density call
runs at 4600-4800 ns per gradient; declining it and letting each subject's
identical scalar density collapse under CSE, feeding a short `ADD_N`
reduction tree instead, runs at 1000-1100 ns, 4.3-4.6x faster, at 5 interleaved
rounds each of N=50,000 gradient evaluations. `dogs_hierarchical`'s decline
splits one 750-dog density into two 375-dog ones (a smaller regime of the
same effect, its outcomes vary more so `distinct` stays close to `Luse`) and
is cost-neutral, 11.1-11.8 us either way at 8 interleaved rounds of N=30,000.
Prep time (compiling and binding the larger declined graph) grows by roughly
a millisecond on both models, negligible against thousands of gradient
evaluations per chain.

## Observation collapse (`collapse.cpp`, fast mode only, disable: `STANLI_NO_COLLAPSE=1`)

A likelihood usually evaluates the same expression many times. In

```stan
y ~ normal(a[county] + b * floor, sigma);
```

with 12,573 houses in 386 counties and a 0/1 `floor`, there are at most 772
different locations, and the model is compiled with the data in hand. This
pass finds terms like that and evaluates each distinct case once. It changes
the order sums are taken in, and for the normal forms the arithmetic, so it
runs only in fast mode (`CompileOptions::fast_math`), after re-rolling has
built the vector terms and before partitioning, CSE and the island carver,
which then work on what is left. The design, the corpus census behind it and
the numerical checks are in
[`notes/performance/2026-10-06-sufficient-statistic-collapse.md`](../../notes/performance/2026-10-06-sufficient-statistic-collapse.md).

**Finding equal rows.** Every element that can reach a density argument or a
target term gets a value number: two elements get the same number only if
they are the same expression over the same parameter elements and
bitwise-equal data. Numbers come from an exact table lookup keyed on the
opcode and the operands' numbers, as in CSE, never from a hash alone. The
pass follows elementwise ops, gathers, index and slice reads, element and
slice stores (per element, through each slot's write history), a data matrix
times a vector (row by row), and any pure op with a scalar result. An op it
does not model gives every output element a number of its own, so it can
only keep rows apart. `-0.0` and `0.0` are different data.

A term is one vector density, or every target term of one density
together: re-roll may leave one loop as several vector chunks and a
remainder of scalar terms, one op per observation, and whether it does is
its own cost decision. Those pieces are pooled, so the rows or groups are
found once over the whole loop and not once per piece. This holds for every
density the pass handles: pieces of a density with a statistic pool into the
group or linear form, any other density into the row form, with each piece
contributing the rows it is first to show and the pieces laid end to end.
Pooling happens only when it keeps fewer rows than the pieces would between
them.

**Three ways to evaluate a collapsed term**, chosen per term:

- *Rows.* For any density with an elementwise form: keep one observation
  per distinct row (outcome and every argument), evaluate the density on
  those, and take a dot product with the row counts. This is the form for
  densities with no sufficient statistic (`neg_binomial_2`, `student_t`,
  `ordered_logistic`, mixtures, a parameter variate).
- *Groups.* For a density whose observations enter only through sums, with a
  data variate: the variate does not need to repeat, only the parameters.
  For `normal` and `lognormal`: Each group of observations that share a location
  and scale is replaced by its count, a centre `c` (the rounded group mean),
  `d = sum(y - c)` and `S = sum((y - c)^2)`, computed once with compensated
  sums, and `OP_NORMAL_GROUPED_LPDF` uses
  `sum((y - mu)^2) = S + 2 (c - mu) d + n (c - mu)^2`, which is exact for any
  `c`. Keeping `d` is what makes this as accurate as the per-observation sum
  when the data sit far from zero; raw moments (`sum(y)`, `sum(y^2)`) lose up
  to all of the digits and are not used.
  For `exponential`, `gamma`, `inv_gamma`, `beta`, `poisson`, `poisson_log`,
  `bernoulli`, `bernoulli_logit`, `binomial` and `binomial_logit`, each group
  is its count and two sums of the data (`sum(y)`, `sum(log y)`, successes
  and failures, as the density needs), and `OP_FAMILY_GROUPED_LPDF` evaluates
  Stan's formula for the group from those. These densities are linear in
  their statistics, so nothing cancels that did not cancel per observation.
  Terms of the data alone (`lgamma(y + 1)`, binomial coefficients) are summed
  once into a constant, and each term is kept or dropped under `propto`
  exactly as Stan does for the arguments that are parameters.
- *Linear.* When there is one scale and the locations are affine in a few
  values (`alpha + X * beta`, written with a matrix product, with one `fma`
  per column, or as `normal_id_glm`), the whole term is a quadratic form.
  With a centre `t` near the least-squares solution and `u = theta - t`,
  `sum((y - mu)^2) = K - 2 u.b + |R u|^2` exactly for any `t`, where `R` is
  the triangular factor of the weighted design. `OP_LINEAR_GAUSSIAN_LPDF`
  evaluates that in `P^2 / 2` multiply-adds whatever the number of
  observations. Because the identity does not depend on `t` being the exact
  minimiser, a rank-deficient design needs no special case. Expanding around
  the least-squares centre is what keeps it accurate near the mode; the raw
  Gram form (`y'y`, `X'y`, `X'X`) and a QR of `[X y]` both cancel there and
  were rejected (measurements in the design note).

**GLM densities.** `normal_id_glm` takes the linear form.
`bernoulli_logit_glm`, `poisson_log_glm` and `binomial_logit_glm` over a data
design take the group form: two observations are one group when their design
rows and intercepts are equal, the linear predictor is formed for the
distinct rows only (those rows of the design times `beta`, plus the
intercept), and the family kernel takes it. `neg_binomial_2_log_glm` has no
statistic and takes the row form, as `neg_binomial_2_log` on the distinct
rows' predictor. `categorical_logit_glm` and `ordered_logistic_glm` are left
alone.

Scalar target terms are handled the same way: terms with equal values are
computed once and multiplied by their count. That is all-or-nothing per
model, on an estimate of op dispatches saved: merging only some of the
repeats left lane partitioning a mix of lanes its cost model priced badly
(`dogs_hierarchical` 0.68x, `multi_occupancy` 0.79x in that state).

**Restricting the arguments.** The kept rows need their arguments at the
kept rows only. The pass pushes a gather up through the ops that built each
argument: `f(a, b)[rows]` is `f(a[rows], b[rows])`, a gather of a gather is
one gather, a gather of data is smaller data, a gather of `X * beta` is the
kept rows of `X` times `beta`, and a vector filled by element stores becomes
a pack of the kept scalars. Each rule has a local proof, including that the
writer's inputs are not written between it and the density. Where no rule
applies, a plain gather of the full vector is always correct; then only the
density gets shorter, and the rewrite is taken back unless the rows are at
least four times fewer than the observations. Where pushing the gather up
would end in one scalar chain per kept row, and the rows are affine in
elements of a few slots (`alpha[county] + beta * floor`), they are built
from those slots with a gather, a multiply and an add per column. A pooled
term's group parameters are built that way where they are affine, and
otherwise taken piece by piece from where each group's first observation
read them. The ops that built the
full-length arguments are removed if nothing else reads them, following
index reads through store chains element by element, so a loop of
`mu[n] += ...` keeps only the updates behind the kept rows.

**When it applies.** A term needs at least 16 observations. Rows must be at
least 2 times fewer than observations where the density's elementwise form
is native (Bernoulli and binomial), 8 times where it costs a recorder call
per element; groups 2 times fewer; the linear form is used when its
triangular products are at most half the work of forming the locations.
`STANLI_COLLAPSE_REPORT=1` prints each term with its observations, rows,
groups, the form chosen or the reason it was left alone;
[`harnesses/collapse_census.py`](../../harnesses/collapse_census.py) runs
that over the corpus.

**Rejection.** The closed forms run Stan's own argument checks under the
original function's name. A point Stan rejects is still rejected. When
several observations are invalid, the one named in the message can differ,
and the linear form never forms the locations, so it checks the values they
are affine in. A collapsed GLM sees only its linear predictor: where Stan
would name a non-finite weight or intercept, it names the predictor, and it
rejects a non-finite predictor in the few cases where Stan's Poisson GLM
returns a non-finite value instead. `neg_binomial_2_log_glm` errors are
reported under `neg_binomial_2_log_lpmf`.

**Measured** (i9-13900K, one P-core, `main` at `f99f3ec7`, fast mode with
the pass on against fast mode with `STANLI_NO_COLLAPSE=1`, paired
`ab_bench_corpus.py`, 9 rounds on an idle machine; the identical-binary
control is within 0.8% on 95% of models and 1.4% at worst): 98 of 352 corpus
models have a term that collapses, and 97 of them could be timed. Their
gradients are 7.8x faster in geometric mean (median 8.3x); 79 are at least
2x faster, 40 at least 10x, and none is slower: `radon_pooled` 385x (37.3 us
to 97 ns), `nes` 179x, the earnings regressions 63x to 109x,
`nes_logit_model` 103x, `diamonds` 69x, `ch12_m12_4` 30x, the hierarchical
radon models 10x to 13x, `election88_full` 10x. Five are within 10% of
unchanged. A model the pass leaves alone has the same op graph with it on
or off.

Preparation pays for the analysis: over all 342 timeable models it is 7%
longer at the median (0.09 ms) and 3% longer in total (12.93 s to 13.34 s).
Among the 98 models it changes the median is 15% (0.39 ms); the most added
to a model of ordinary size is 42 ms, on `ch12_m12_6` (162 ms before), whose
terms it examines and leaves alone. Six models it leaves alone prepare more
than 1.5x slower, by 1 ms to 7 ms (`hier_2pl`, `prophet`, `lsat_model`,
`bym2_offset_only`, `wells_dist`, `irt_2pl`): grouping their observations
is most of a short preparation. Sampling eighteen of the collapsed models
with four chains gave posterior means within 0.12 posterior standard
deviations of default mode on every parameter (`election88_full` the
largest), and whole runs up to 50x shorter. The fast-mode gate in
[`TESTING.md`](../../TESTING.md#fast-mode) passes with the pass on; the
largest deviation in the corpus is `election88_full` at 1.6e-13.

Default mode against `main` before the pass (`614db5e1`), 339 models, 7
rounds: geometric mean 0.9999. 24 models read more than 2% slower and 19
more than 2% faster; with 15 rounds 13 of the 24 still do, by 2% to 6%
(`aalto_lin` 0.94, `ch14_m14_1` 0.94). Their default-mode op graphs are
identical at every pass stage, and an earlier build that added one unused
function to the base moves the same models by as much, so this is code
placement. `ch12_m12_4` reads 0.91 and is bimodal (82 us or 108 us from run
to run) in every build, the base included.

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
the shapes of immediates, never which slot a lane reads), and each bucket
is rewritten in place of its first lane. Because the fingerprint ignores the
slots, lanes that differ only in constants, or in indices into one shared
base, share a bucket. That includes the two branches of a data-dependent
condition when they differ in that way. After bucketing, every operand must
be the same slot in every lane, a constant per lane, or an indexed read of
one base. A bucket in which an operand is a different non-constant slot in
different lanes (two branches that use different parameters) splits into one
bucket per slot at that operand, with all constants in one bucket, and each
piece goes through the same checks. Lanes that each read their own slot, as
a recurrence does, leave pieces shorter than four lanes, which decline.

What a bucket's ops read is proven rather than assumed: no slot the fused
ops read from outside may be written while the run is in flight. That one
rule carries three soundness obligations at once (no cross-lane
recurrence, no writer in the middle of the bucket, no destructive
in-place store between the lanes), because all three appear as a writer
between the first lane's position and the last lane's end. Ops only ever
move earlier, so an in-place store already proven safe against a later
reader stays safe, and a bucket that trips the rule splits at the writer
rather than declining.

Ops only move earlier, so a print, a reject or an argument check between two
lanes of a bucket would run after work that followed it in the source, and a
reject could report a later density's error instead of its own message. A
bucket splits at such an op, and at any op with a payload (a compiled region,
a solver with a callback) unless it is a region proven free of effects. The densities that are effectful only because
they check their arguments (`categorical`, the integer-outcome check) are not
barriers: their errors reorder like those of every other fused density.

In the default mode a bucket also declines when several of its operations read
one active scalar. Grouping those contributions by opcode can lose many ULP
against the source order, which the unrolled graph keeps. Fast mode skips this
check (`fuse_shared_params`).

A bucket is rewritten in one of these forms:

- One template, one bucket: each input becomes a slice when the lanes
  read a contiguous range, a gather when they do not, and a shared
  operand when every lane reads the same slot. Contiguity is a cost
  question here, not a legality one, decided by the same affine-run
  classifier reroll.cpp's row lanes use. An operand as wide as the
  lane but shared by every lane instead tiles via `OP_REP_MAT`
  (reroll.cpp's tiling exactly), hoisted once instead when every input
  is shared, not only this one.
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
  retried to a fixed point. The cap of 32 (`kMaxSplits`) counts every split in
  the whole pass, across all buckets, and the lane report names it when it
  ends a bucket. It is reached in 18 corpus models, but neither a budget per
  bucket nor no cap at all changes any corpus program (2026-11-20).
- One arm rewrites a chain instead of widening it. A lane that multiplies
  two scalars, subtracts a width-m vector, prepends a zero, cumulatively
  sums, and hands the result to a scalar-outcome categorical is one row
  of a `categorical_logit_glm_lpmf`, because
  `cumsum([0, ta - b_1, ...])[j] = j*t*a - sum_{i<=j} b_i`. Buckets refine
  on the subtracted vector (the item), and the slope is whichever of the
  two scalars that refinement holds still.
- Lanes that each end in a scalar-outcome categorical over their own row
  (brms categorical: one `categorical_logit` per observation) become one
  categorical with `kCategoricalRows` set. It takes the lanes' outcomes as
  one vector and their rows end to end, runs the per-row rule unchanged,
  and adds the rows' lps left to right, so where those terms lead the
  target the result is bitwise the per-row graph's.

Fusing is not always a win, so each bucket is costed before it is
emitted, in the currencies of `pass_util.hpp` (`kLaneOpCost`,
`kLaneDensityElem`, `kLanePartitionMargin`, the same ones that reroll.cpp's
own region pricing uses). The island carver uses the same per-op cost
(`kLaneOpCost`) with its own element weights in `island.cpp`. The currencies are about 5 ns per graph op against about 1 ns per
element moved, and a density element is charged `kLaneDensityElem = 6`
element units, a little more than one op dispatch. The reasons for that reading
are in
[`notes/performance/audit-2026-11-20-open-questions.md`](../../notes/performance/audit-2026-11-20-open-questions.md).
Two measured pessimizations live in that model rather
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
34,634 to 91 (6.5x) and `grsm_latent_reg_irt` (6.3x). The general rules do
not cover the GLM arm's shape (a scalar minus a vector per lane, then
`CONCAT2` and `CUMSUM`): with the arm turned off, those two models prepare to
50,611 and 28,005 ops instead of 101 and 81 (2026-11-20).

### Why a group of lanes stays unfused

Re-rolling and partitioning record one reason for every group of matching
lanes that they leave unfused (`runtime/include/stanli/lane_decline.hpp`).
`STANLI_LANE_REPORT=1` prints them as `LANES {...}` lines with the graph, the
op, the lane count, the period (re-rolling) and the opcode, plus a line with
the number of target terms listed more than once. Re-rolling tries several
periods at one position and reports each attempt that counted enough lanes.
`tools/lane_declines.py` prepares every corpus model under the switch and
ranks the reasons by models and collections;
[`notes/performance/2026-11-20-lane-declines.md`](../../notes/performance/2026-11-20-lane-declines.md)
has the table.

## Eliding full-extent stores (`inplace.cpp`, disable: `STANLI_NO_INPLACE=1`)

Re-rolling and partitioning can leave a store that overwrites a whole
destination (`v = tmp;` as one slice store, or an identity slice of the whole
value). That store is a copy. The pass renames the later readers of the
destination to read the copied value and deletes the store. The conditions are
the ones that keep the numbers the same. The copied value has exactly one
reader, which is the store, and neither it nor the destination is a parameter
or a root. The next write to the destination, if there is one, neither reads
what the store put there nor leaves adjoints for the reverse sweep to carry
past it; a store that covers the whole destination and writes in place
qualifies. An op that holds its inputs in a payload the rename cannot reach (an
island, a loop, a region map or a `reduce_sum`) and that reads the destination
blocks the rewrite. With one reader, the reverse sweep's additions keep the
grouping they had. The pass runs after every pass that emits slice stores and
before CSE and islands. It has no switch of its own: `STANLI_NO_INPLACE` turns
it off together with the in-place updates.

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

In the default mode, an active duplicate normally shares only its value and
keeps its own pullback, so the reverse sweep accumulates in source order. In
fast mode (`merge_active`) every active duplicate merges completely, and the
rest of this paragraph does not apply. A duplicate
whose seed is a whole number is different. The target seeds each term
with 1 and a scalar addition passes its seed on unchanged, so a copy that
reaches the target only as a term or through additions has a whole-number
seed, and adding such seeds is exact. Its k copies merge into one op with
one pullback, and a term listed k times becomes one term multiplied by k.
That rounds once where adding the copies rounded k - 1 times. A brms
`hurdle_poisson` model with 5,308 zeros among 12,000 rows adds
`bernoulli_lpmf(1 | hu)` once per zero; its log density was 588 to 1,267
ULP from an 80-digit reference (CmdStan: 102 to 567) and is now 3 to 56,
and a gradient runs 1.24x faster. Merging through additions matters for
the gradient: in `s2_hurdle_cumulative` each nonzero row adds
`bernoulli_lpmf(0 | hu)` to its own term, and merging only the zeros'
terms left those 26 pullbacks accumulating against one large one (9.6 ULP
on one entry, against 3.4 before). With both sides merged it is 0.4 ULP.

A branch can receive an argument that it never reads: the `hurdle_poisson`
model above passes each zero's `mu[n]` to a branch that ignores it. So the
pass also drops a static `INDEX`, `SLICE`, `SLICE_STRIDED` or `GATHER` whose
value nothing reads, and a copy that shares another op's value once nothing
reads it (merging terms leaves the copies that computed their arguments
unread). It drops no other unused op, since an unused density can still
reject.

## Native scalar probability categorical (`message.cpp`)

The active scalar-outcome probability form of `categorical_lpmf` has a narrow
derivative: only the selected probability receives the incoming seed divided
by that probability. Its kernel therefore asks Stan Math's double overload to
compute the value and perform every domain and bounds check, then applies that
single pullback directly. This removes the nested reverse-mode tape from both
sweeps without changing the graph or its operation count. Both categorical
families enter this kernel through the shared density registry; probability
versus logit, scalar versus array outcome, activity, and propto are ordinary
variant bits rather than an opaque per-op payload.

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
medians, not the later full-corpus warmed means of 0.121131 ms (11.04x CmdStan)
and 0.070870 ms (10.75x CmdStan) after lane partitioning in
`docs/internal/artifacts/corpus-bench.tsv`.

## Compiled scalar generated-quantities RNGs (`rng.cpp`, `wa_interp.cpp`)

The write-array graph used to stop at every RNG call. Drivers then selected
`WaInterp`, which starts the whole section from statement zero for every saved
draw; even a late scalar draw therefore reinterpreted constrained parameters,
transformed parameters, and all earlier generated quantities.

`OP_RNG` now covers the 24 scalar RNG families in the `ScalarRng` enum
(`rng_family.hpp`): the first tranche was `poisson_log_rng`, `uniform_rng`,
`bernoulli_rng`, `normal_rng`, `lognormal_rng` and `binomial_rng`, and later
ones added more, among them `gamma_rng`, `student_t_rng`, `beta_binomial_rng`
and `neg_binomial_2_log_rng`. The graph and interpreter
share one draw helper, so they call the same Stan Math function in the same
order on the same caller-owned `WaRng`. An executor holds only a temporary
pointer to that chain resource for one forward sweep; scoped restoration and
copy rebinding prevent pooled or interleaved executors from retaining another
caller's stream. The graph passes treat every RNG op as an ordered effect, so
they cannot fold, reroll, carve, delete, or duplicate a draw whose result is
otherwise unused.

The boundary is deliberately fail-closed. Apart from the container-valued
families below (`categorical_rng`, `categorical_logit_rng`,
`poisson_binomial_rng`, `dirichlet_rng`, `multi_normal_rng` and
`multi_normal_cholesky_rng`), container-valued results or unsupported RNGs
still select the whole-section interpreter. Scalar
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
`OP_PROD_VEC` has forward and backward kernels. In generated quantities it
uses Stan Math's Eigen product grouping while hiding the graph arena's input alignment for
materialized vectors, so an odd slot offset cannot change their packet
boundary. A strided matrix-row operand instead records and preserves Stan
Math's ascending scalar grouping before the expression is materialized.
Lowering admits any nonempty vector or row-vector value whose reduction
grouping the kernel can reproduce (`reduction_grouping`); an argument whose
grouping is not known, and an empty container, stay on the existing
fallback. The opcode carries the `kVariantGrouped` trait, which excludes it
from reroll matching and island CALLs.

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
scalar result. It reuses `OP_RNG`: the scalar-family variants retain their
scalar double inputs, while the categorical variant reads one variable-length
vector descriptor and writes the one-based Stan int exactly into one double slot. It
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
Math therefore remains the single definition of categorical probability-vector
empty-input and simplex
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
results do not refresh `docs/internal/artifacts/corpus-bench.tsv` or the generated full-corpus
table in `docs/benchmarks.md`.

## Compiled generated-quantities extrema (`elementwise.cpp`, `lower.cpp`)

`OP_EXTREMA_VEC` has `min` and `max` variants with forward and backward
kernels. The backward kernel adds the output adjoint to the selected element.
The argument may be a vector, row vector, matrix, one-dimensional real array
or one-dimensional integer array, and the two-argument integer form of `min`
and `max` has its own scalar op. The argument must be the whole container that
the overload names; a slice or any other part of a container is refused. The
lowering looks at the layout of the lowered argument, whatever the MIR
expression looks like, so an expression that produces a whole container
qualifies when its evaluation grouping is one the kernel reproduces
(`reduction_grouping`). The kernel uses
the same address-independent grouping that the pinned Stan Math
implementation applies to an owning Eigen value, while retaining its
empty-real results: positive infinity for `min` and negative infinity for
`max`. An argument whose grouping is not known, and any other call that
the lowering cannot place, remain in `WaInterp`.

The opcode carries the `kVariantGrouped` trait, because its variant bits pick
the reduction grouping. That excludes it from reroll matching, CSE, collapse,
partitioning, and interpreter-island CALLs.

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
`docs/internal/artifacts/corpus-bench.tsv` or the generated full-corpus table in
`docs/benchmarks.md`.

## Compiled covariance-form multivariate-normal RNG (`rng.cpp`, `wa_interp.cpp`)

The vector-result `multi_normal_rng(vector, matrix)` write-array surface reuses
`OP_RNG` rather than adding a second effect vocabulary. Its variant reads a
length-`K` mean and a column-major `K` by `K` covariance, and writes a
length-`K` vector. Lowering admits only an exact non-array `UVector` result,
one non-array `UVector` mean, and one square `UMatrix` covariance whose known
dimensions match. Row-vector/array overloads and mismatched or unknown shapes
still select `WaInterp`. Single-vector square `multi_normal_cholesky_rng`
now uses the same compiled route; see the
[remaining RNG coverage follow-up](../../notes/execution/2026-09-28-vector-rng-implementation.md).

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
do not refresh `docs/internal/artifacts/corpus-bench.tsv` or the generated full-corpus table.

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
These targeted medians do not refresh `docs/internal/artifacts/corpus-bench.tsv` or the generated
full-corpus table.

## Control flow that depends on a parameter (`lower.cpp`, `mir_prog.hpp`)

`if (theta > 0) ...` cannot become ops: the op list is fixed at load
time, and this statement picks its branch anew at every evaluation. The
region (just the conditional, not the model around it) is translated
into the same instruction list the ODE right-hand sides use, and one op
runs it: on plain numbers for the value, and for the derivatives either
a generated backward (the same one that tape islands use, below) or, when
none can be generated or `STANLI_NO_NATIVE_ADJ` is set, a replay under
stan-math's own autodiff. Both give the derivatives of whichever branch
ran. That is what CmdStan's generated C++ does for the
same statement. This is not an optimization and no switch turns it off;
without it the model does not run. A loop over many observations whose
body branches on a parameter has its own op, described in the next section.

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

## Parameter-branch loops over many observations (`lower_region_map.cpp`, disable: `STANLI_REGION_MAP=0`)

A `for` loop whose body branches on a parameter used to have two lowerings.
Lowering could unroll it and compile one small region per observation, which is
correct under any branch pattern but compiles N programs and leaves N ops.
Or the retained loop (`OP_LOOP`, below) recorded one pass and replayed it,
which is fast until an observation changes branch and then re-records the whole
loop, which the design note measured at about thirty times the cost of a replay. Under sampling the parameters
move on every step, so observations near a threshold flip and the loop
re-records on nearly every gradient.

`OP_REGION_MAP` is one graph op for the whole loop. Its body is compiled once
into a register program, by the compiler that handles the regions above, with
the loop variable held in a register. The op runs that program for each
iteration and adds up the target terms. Its backward is a generated adjoint
program. It records nothing, so it does not matter which branch each
observation takes. The op is on by default.

Lowering maps a loop when all of these hold (`lower_region_map.cpp`):

- It is the log-density graph, the loop is the outermost one, the bounds are
  fixed at lowering, and the loop runs at least twice. A model compiled with
  `STANLI_STRUCTURED_LOOPS=1` or `=force` never maps. A short loop maps too.
  Unrolled, such a loop gets one island per iteration, so it pays one graph
  op and one program dispatch per iteration where the map pays one op and
  runs its body once per 64-lane tile. Two costs do not shrink with the trip
  count. The tile holds 64 lanes of the forward registers, and the body is
  compiled for every iteration, so it reads by index what the unrolled form
  knew at compile time. A 3-trip outer loop over a 100-trip inner loop (an
  experiment, not a corpus model) became one map of 2,002 instructions with
  300 dynamic-index calls per lane and a tile of about 616 KB, against 300
  islands of about 19 instructions.
- The body branches on a parameter: an `if`, a ternary, an `&&` or `||`, or a
  `while` whose condition depends on one. A name from outside the loop depends
  on a parameter when its lowered value does. A name declared in the loop or in
  a called user function depends on one when a value assigned to it does, with
  each argument's dependence taken from the call site. The MIR's AD level is
  not used, because stanc3 marks `real t = x[n];` as autodiff although `t`
  only holds data. Lowering decides a branch on data when it builds the graph,
  and mapping such a loop made it about 14x slower.
- Iterations are independent. Everything the body assigns is declared inside it,
  with one exception: a real declared outside may collect the terms, if the
  body only adds to it and never reads it. Nothing escapes with `break`,
  `continue` or `return`, the body has no effect (`print`, `reject`, an RNG
  call), no enclosing loop scales the target, and no call returns several
  values.
- The body compiles to at most 16,384 instructions, and a generated backward
  exists for all of them.

A loop that is refused lowers as it did before. A loop that carries a value
between iterations stays on the retained-loop path.
`STANLI_REGION_MAP_DIAGNOSTICS=1` prints the reason for each refusal and the
form each mapped loop got.

There are three forms. In the lane form, iterations run in tiles of 64: each
instruction runs once over a tile instead of once per iteration, and a branch
splits a 64-bit lane mask so that a block runs for the lanes that reach it and
is skipped when none do. In the scalar-save form, one iteration runs at a time
and its registers are saved for the backward. In the scalar-recompute form the
backward reruns each iteration's forward instead of reading saved registers;
lowering picks it when the saved state is large, more than 1,024 cells per
iteration or more than 2^24 cells in all (`STANLI_REGION_MAP_SAVE_LIMIT` changes
the second limit). Lowering tries the lane form first and falls back to the other two when the
lane plan refuses the program, for example because it holds an opcode that the
lanes do not batch. `STANLI_REGION_MAP_LANES=0` skips the lane form.

A read such as `X[n]` or `z[, g[n]]` of a container that every iteration
shares reads the selected elements where the container lies in the lane form,
and its backward logs one addition per selected element (below). Neither the
forward nor the backward copies the container into the call's window, so the
work per lane grows with the number of selected elements and does not depend
on the container's length. A container that no parameter reaches gets no
adjoint at all. Lowering marks a live-in of the map active only when a
parameter reaches it, and the adjoint generator then clears the input adjoint
of an index read or write, a dot product or a sum whose input no parameter
reaches. Islands get the same masks from the carver's activity. Re-rolling
folds issue #657's loop, so for that loop the lane form is only the fallback
(below).

The lane form checks the index of each read in every lane. The backward does
not check again, because the forward of the same evaluation checked the same
index values. The forward skips its checks too when the index values are the
same in every evaluation: they come only from the loop variable, constants and
data, and no branch on a parameter decides whether the read runs
(`find_proven_gathers` in `region_map_lanes.cpp`). The first evaluation of each
executor runs every check. When it succeeds, the region map sets one cell of
its scratch, and later evaluations of that executor skip the checks. An
out-of-range index therefore throws the same error at the same evaluation as
before, and keeps throwing, because a failed evaluation never sets the cell.

Each iteration computes the same scalar operations as the unrolled graph. The
op adds the iteration terms in order and then adds that total to the rest of
the target, which regroups the sum within the ULP policy. In the lane form,
iterations that add into one shared adjoint cell (a scalar parameter that every
iteration reads) log their additions, and the tile replays them in scalar
order, so the lane gradient equals the scalar one bit for bit. An index read
such as `M[n]` from a shared matrix logs one addition per selected cell. It
used to log one per cell of `M`, which made a 572-row loop over a 572 x 3
matrix about 40x slower than the unmapped loop. The design, the
lane plan and the measurements are in
[`notes/execution/2026-10-03-region-map-design.md`](../../notes/execution/2026-10-03-region-map-design.md),
[`2026-10-02-region-map-lanes.md`](../../notes/execution/2026-10-02-region-map-lanes.md)
and
[`notes/performance/2026-10-03-cogmod-sampling.md`](../../notes/performance/2026-10-03-cogmod-sampling.md).

## Keeping each chain's program small (`lower.cpp`, disable: `STANLI_PROGRAM_BUDGET=0`)

Every chain has its own executor, and every executor holds its own copy of
the program: for each op, the op itself, a 336-byte kernel context, a forward
entry and a backward step, about 450 bytes in all (`Executor::program_bytes`).
Values and adjoints come on top. A loop that unrolls to 100,000 ops therefore
costs about 45 MB in every chain. With twelve chains on one machine, unrolled
programs of 10 to 24 MB per executor made each gradient 2.4 to 3.0 times as
slow as one chain alone, while programs up to 5 MB cost 1.1 to 1.3 times
([boundary study](../../notes/triage/2026-11-24-issue-657-region-map-boundary.md),
issue #657).

Lowering chooses the form of each outermost loop in this order:

1. It unrolls the loop and lets the passes fold it. When re-rolling folds the
   loop into vector ops, the program no longer grows with the data, and this
   is the fastest form on one chain and on twelve.
2. After the passes, if the program of the log-density graph is larger than
   8 MiB, lowering charges each unrolled loop the ops that it produced and
   that survived the passes. Then it lowers the graph again with the largest
   of these loops in a compact form, until the rest fits. It keeps the second
   graph only if its program is smaller.
3. The compact form is the region map's lane form, when the region map can
   take the loop's shape. Here the loop does not need to branch on a
   parameter.
4. If the program is larger than 32 MiB, a loop that the lane form refuses
   becomes `OP_LOOP` instead.
5. Otherwise the loop stays unrolled.

A loop that branches on a parameter keeps the form that the sections above
describe. So does a loop that prints, rejects or counts into an integer
declared outside it.

The two budgets come from measurements of the corpus models that a single
8 MiB budget would change (2026-11-24, Apple M3 Ultra, load 6 to 19 on 32
cores; time per gradient of the compact form against the unrolled one, with
each copy on its own thread). The rows marked * were measured on the build
with only the 8 MiB budget and plain sums, before these models stopped
changing; the others on the final build. The
`examplemodels_probit_multi_good` row was measured again on main at
5fcbdee5a. On the earlier base it was 1.45x, 1.33x and 2.60x faster. In a
profile on 5fcbdee5a, the Eigen matrix product in the lane form's register
program takes about a sixth of its gradient.

| model | unrolled program | compact form | 1 copy | 4 copies | 12 copies |
| --- | ---: | --- | ---: | ---: | ---: |
| `examplemodels_probit_multi_good` | 12.9 MB | lane form | 1.09x faster | 1.01x faster | 2.00x faster |
| `hbayesdm_prl_rp` * | 9.0 MB | `OP_LOOP` | 1.29x slower | 1.39x slower | 1.08x faster |
| `iohmm_reg` * | 17.3 MB | `OP_LOOP` | 1.18x slower | 1.32x slower | 2.35x faster |
| `examplemodels_cjs_cov_raneff` * | 22.0 MB | `OP_LOOP` | 1.17x slower | 1.16x slower | 2.81x faster |
| `ch14_m14_11` * | 30.0 MB | `OP_LOOP` | 1.52x slower | 1.27x faster | 1.33x faster |
| `ch12_m12_6` | 44.3 MB | `OP_LOOP` | 1.01x slower | 1.84x faster | 2.31x faster |
| `nn_rbm1bJ100` | 59.2 MB | `OP_LOOP` | 1.00x | 1.04x slower | 1.00x |
| `examplemodels_ms` | 66.7 MB | `OP_LOOP` | 1.20x faster | 1.69x faster | 1.55x faster |

The lane form was about as fast as unrolling on four copies and faster on one
and twelve. For the programs of 9 to 30 MB, `OP_LOOP` was 1.2x to 1.5x slower
than unrolling on one copy and mostly on four, though faster at twelve. For the programs of 44 to 67 MB it
was within 4% of unrolling or faster on one and four copies, and 1.0x to
2.3x faster at twelve. Four chains are the common case, so `OP_LOOP` waits
for the second budget. With both budgets, four corpus models
change: `examplemodels_probit_multi_good` takes the lane form, and
`ch12_m12_6`, `examplemodels_ms` and `nn_rbm1bJ100` take `OP_LOOP`. Their
log-density programs shrink from 13 to 67 MB to under 0.2 MB. The models that
stay over 8 MiB have no loop that a compact form takes: the loop is nested
inside another loop, or the compact forms refuse it.

The compact forms add the iteration terms with compensation once there are
more than 32 of them (`CompensatedSum` in `pairwise_sum.hpp`), not in plain
order. On a loop whose terms nearly cancel, plain order is off by about
1e-9 in relative terms and the compensated sum by about 1e-12. The unrolled
graph adds such terms in plain order, so a compact form can move the log
density by more than 10 ULP from CmdStan while moving it closer to the exact
value. `tests/ulp_ledger.json` lists the two corpus models where this
happens on Darwin arm64. For `examplemodels_probit_multi_good`, the 80-digit
reference puts stanli 150 and CmdStan 225 ULP from the true log density. For
`nn_rbm1bJ100`, where the 80-digit evaluation timed out, an exact sum of the
60,008 terms puts stanli 0 to 15 ULP and CmdStan 11 to 3,983 ULP from it.

A loop that unrolls to more than 2^20 ops before the passes stops the first
lowering, which starts again with that loop in a compact form, `OP_LOOP`
included. The largest loop in the corpus unrolls to about 180,000 ops, so
the ceiling stops only much larger loops, such as one over ten million rows.
If no compact form takes such a loop, the last attempt unrolls it anyway.

The cost is a second lowering for the models over the budget. An
out-of-range index raises the same error in every form of a loop: each
dynamic index read carries the label of Stan's error for its selector
(`label_dynamic_index` in `structured_loop.cpp`), so the region map and
`OP_LOOP` raise the error that the unrolled graph raises, such as
`vector[uni] indexing: accessing element out of range`. In the region map
and `OP_LOOP`, a range selector or an indexed assignment out of range still
raises `structured index out of range`.

| switch | effect | source |
| --- | --- | --- |
| `STANLI_PROGRAM_BUDGET=n` | the program budget in bytes, default 8 MiB; 0 turns the budget off | `lower.cpp` |
| `STANLI_LOOP_BUDGET=n` | the program size in bytes past which a loop may also become `OP_LOOP`, default 32 MiB | `lower.cpp` |
| `STANLI_UNROLL_CEILING=n` | the most ops an outermost loop may unroll to before the passes, default 2^20; 0 means no ceiling | `lower.cpp` |

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

The adjoint generator keeps every forward instruction and only adds
registers. So the forward part of the estimate is a lower bound on the
whole, and a run whose graph cost is below that bound stays on the graph
without generating its adjoint. The decision is the same as with the full
estimate: no corpus model's prepared program changes. It takes the island
pass on `examplemodels_ms` from 263 to 131 ms. `STANLI_DEBUG_ISLAND` prices
every run in full, so its estimate lines keep their values.

The constants behind that estimate (5 for `kOpCost`, 2 for `kValueRegWeight`,
and a bare 3 for each live-out element in `island.cpp`) are calibrated on corpus
timing, not on instruction dispatch. A micro-benchmark of one graph op
against one island instruction and of a live-in and a live-out element
measured ratios of 1.458, 0.131 and 0.684, and setting the constants to
those values (rounded to 1) lost 46 islands across a 254-model corpus run
and made `iohmm_reg` 1.82x slower per gradient; the constants stand for what
an island saves beyond dispatch, and the only recalibration that holds is a
corpus sweep.

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
the estimate charges it accordingly. Elementwise vector ops compile at any
width, with length-1 operands broadcast, and a run that holds them is priced
both whole and split at them. A call must have a single-number result, so any
other op with a vector result still ends a run.

CSE (above) can let copies of an op share its value. The carver used to
leave the op that computes the shared value on the graph. A recursion that
reads the same element several times, as `iohmm_reg`'s forward step reads
`logA[t][i]` once for each j, has such an op every few ops. No run reached 32
ops, the recursion stayed as graph ops, and CSE made `iohmm_reg` about 2x
slower. Now a run takes shared ops too, and the island computes each copy
again, which is what the graph without CSE does. A run stops before a shared
op that has a copy after the run. That op stays on the graph, and the later
copies keep sharing its value. Such a cut can still put a shared op in an
island and leave some of its copies on the graph. The first of those copies
then computes the value for the others. A shared op that the island could
only run as a call to its graph kernel stays on the graph. The call costs what
the graph op costs, while on the graph each copy skips its forward. Taking 29
shared `lgamma` calls into one island made `epinow2_estimate_infections`
1.06x slower. For the same reason the estimate charges a copy on the graph
half an op, since it runs only its backward. A run that is mostly copies can
then stay on the graph: 44,354 of the 68,252 ops in `ch14_m14_11`'s one run
are copies, and its graph with CSE is faster than the island on an idle
machine.

A region with more distinct inputs than an op's six descriptors packs the
leading ones with an `OP_CONCAT2` chain, as necessity lowering does, and
the estimate charges each link as one op plus the elements it copies forward
and back.

The pass still refuses outright: short runs (under 32 ops), densities in the
dropped-constants form (see the refusal above), and regions producing target
entries. The generated
backward supports forward-only branches by recording which basic blocks ran
and reversing only those blocks. Back edges, malformed jump targets,
unsupported derivatives, and paths that can read an uninitialized register
retain the autodiff replay.

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

A `multi_normal_cholesky_lpdf` with one observation (not an array of vectors),
data `y` and `mu`, an active Cholesky factor and `propto=true` has an analytic
matrix partial in Stan Math, but the generic kernel rebuilt an elementwise var
matrix and nested tape in both sweeps. A Gaussian-process regression with a
single response vector, such as `gp_regr`, has this shape.

That instantiation (`variant=0x84`, selected by the kernel's `mnc_path`) now
evaluates Stan Math's
triangular inverse, half-vector, value, and full factor-partial matrix in the
same expression order during forward, retaining the partial matrix in
scratch. Backward is only the seeded accumulation into the factor. All other
activity masks, vectorized observations, and non-propto calls keep the generic
Stan Math replay, except that widths 1 and 2 take their own fixed-size kernel
(`kMncSmall`). On `gp_regr` the density falls from 2.48 to 0.70 us and the
whole gradient from 6.05 to 4.20 us (1.44x internally, 1.12x CmdStan).

## Matvec through Eigen gemv (`elementwise.cpp`, `lower.cpp`)

`OP_MATVEC` used to be a hand-written loop that summed each output element
over columns in ascending order, matching Stan Math's `Matrix<var>` multiply
bitwise. On `prophet` (1169x25 and 1169x34) that was 82-83% of the bare
gradient.

The kernel is now `Eigen::Map` forward and transpose-backward gemv. A
2026-09-04 --bare A/B (2000 iterations, four matched runs each) moved
`prophet`'s gradient from 115.5M to 74.3M ns (1.56x; the op itself 97.3M to
55.7M ns, 1.75x), `accel_splines`'s from 13.4M to 10.4M ns (1.28x; the op
7.2M to 3.9M ns, 1.85x), and `accel_gp`'s from 13.6M to 11.3M ns (1.21x; the
op 5.0M to 2.6M ns, 1.90x).

Every affected corpus model's worst ULP against the CmdStan references went
down (`accel_gp` 12 to 2, `accel_splines` 119 to 3, `prophet` 11520 to 256),
since CmdStan runs the same Eigen gemv. Against the previous kernel the
change is 1-4 ULP per element, inside the 10 ULP reassociation budget.

## Compiled ODE right-hand sides (`ode_prog.cpp`)

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
supported); the model's interpreter-fallback warning names the function. A test runs
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
CmdStan columns in `docs/internal/artifacts/corpus-bench.tsv`.

### Direct coupled RK sensitivities (`ode.cpp`, disable: `STANLI_NO_ODE_DIRECT_RK=1`)

An eligible RK45 or Cash--Karp solve now keeps the entire sensitivity system
in plain doubles. Its coupled state has the same layout and evolution as Stan
Math's pinned RK implementation:

```text
z              = [y, S_y0 lane 0, ..., S_theta lane P-1]
y'             = f(t, y, theta)
S_y0'          = J_y S_y0
S_theta'       = J_theta + J_y S_theta
```

The complete coupled vector still enters the adaptive error norm. The runtime
uses the pinned Dopri5 or Cash--Karp Boost tableau, controller, initial step,
output schedule, tolerances, and maximum-step checker. For each callback it
runs the compiled RHS once on doubles, runs its generated reverse once per RHS
output to obtain `J_y` and `J_theta`, and propagates the active sensitivity
lanes in Stan Math's scalar order. Thread-local register, adjoint, Jacobian,
state, and time buffers are reused across callbacks. The observer writes
solution values and the dense solution Jacobian directly into the `OP_ODE`
output and scratch regions; the existing `OP_ODE` backward remains the same
matrix-vector pullback.

Eligibility is deliberately narrower than either the RHS register compiler or
`gen_adjoint`. The RHS must compile, use RK45 or CKRK, contain no runtime
control flow, and consist entirely of an explicit exact-grouping opcode
whitelist. A newly added opcode therefore fails closed. Jumps, `DOT` (whose
double packet reduction can group differently from the `var` replay),
`FMAX`/`FMIN` (whose signed-zero tie choice differs), kernel calls, densities,
and other unproved operations keep the current Stan Math solve. BDF and Adams
also keep that solve. Data-only ODEs retain their cheaper ordinary double
solve because they have no sensitivity lanes to accelerate.

The generated payload is made from a clone of the canonical RHS. The
canonical program is never checkpoint-mutated and remains the exact nested
autodiff oracle and structural fallback. `STANLI_NO_ODE_DIRECT_RK=1`, read when
the model is lowered, selects that oracle while still building the same
payload and recording the same eligibility/refusal result. This makes the
switch both an emergency escape hatch and a same-binary A/B control. Runtime
shape inconsistencies also fail closed to the oracle. On the direct path,
validation order, public function labels, and the translation of Boost's
no-progress failure reproduce the corresponding Stan Math exception type and
message.

This is not the stopped Phase 0 generated-callback experiment. Phase 0 put
generated Jacobian rows behind the existing Stan callback by constructing
`precomputed_gradients_vari` outputs, and its complete callback measured only
0.853x and 0.930x as fast as the canonical callback. The admitted path creates
no callback `var`, arena operand array, or precomputed-gradient node. It uses
the generated reverse only as a double-space local-Jacobian provider inside a
separately owned coupled RK solve; the Phase 0 bridge remains unimplemented.

On 2026-08-28, an uninstrumented Release build on an Apple M3 Ultra
(`-O3 -ffp-contract=off`) ran 15 alternating paired batches of at least 0.5 s
per arm through the integrated implementation:

| model | current oracle / direct RK | paired 95% CI | pinned CmdStan default / direct RK | pinned CmdStan `--O1` / direct RK |
| --- | ---: | ---: | ---: | ---: |
| `lotka_volterra` | 1.8615x | [1.8380x, 1.8853x] | 2.1158x | 2.1241x |
| `soil_incubation` | 1.8035x | [1.7578x, 1.8505x] | 2.2124x | 2.1454x |

The ineligible branched BDF control, `one_comp_mm_elim_abs`, was unchanged at
1.0050x [0.9995x, 1.0106x]. Before integration, the complete-solve ceiling
prototype measured RK45 at 1.800x/1.796x and CKRK at 1.821x/1.860x on the two
eligible real models. At all three evaluation points, each eligible model
matched every solution and Jacobian bit and every callback count; all 18
branchless synthetic shapes won under both RK tableaux. These macOS arm64
results admit the default-on policy, but they are not cross-architecture
evidence. Exact default-versus-oracle
parity and the focused ODE suite on Linux x86-64 are a merge gate. A Linux
failure must narrow the whitelist or retain the oracle, not weaken the bitwise
gate.

## Retained loops

`OP_LOOP` keeps a loop whose body has a `while` or a parameter branch as one
graph op (`lower.cpp`, `structured_loop.cpp`, `structured_recording.inc`). A
loop of independent iterations with a parameter branch goes to
[`OP_REGION_MAP`](#parameter-branch-loops-over-many-observations-lower_region_mapcpp-disable-stanli_region_map0)
first, and only a loop that op refuses reaches this path.
In `write_array` it also keeps a loop that carries a value between
iterations once the graph is large (`write_array_loop_retained`).
[How it works](../../docs/how-it-works.md#retained-loops) explains selection,
recording and guarded replay. Large recordings are sealed into per-iteration
numerical frames, and frame admission enables a temporary recording program
with data-only specializations. A trip whose program and recorder state
repeat an earlier trip's is built from that trip's frame instead of being
recorded: its data operations rerun with relocated operands, skipping those
whose inputs repeat, and its frame then runs forward. The
[recording-site experiment](../../docs/internal/archive/plans/2026-09-20-ctsem-index-recording.md)
and the [ctsem memory record](../../docs/internal/archive/plans/2026-09-19-ctsem-memory.md)
have the proofs and ctsem measurements.

| switch | effect | source |
| --- | --- | --- |
| `STANLI_STRUCTURED_LOOPS=0` / `=1` / `=force` | retain no loops / prefer retaining loops / retain every loop; `auto` or unset is the default policy | `lower.cpp` |
| `STANLI_NO_STRUCTURED_REPLAY=1` | walk the control tree on every evaluation | `structured_loop.cpp` |
| `STANLI_NO_STRUCTURED_CELL_CONSTANTS=1` | disable the per-cell data proof for updated containers | `structured_loop.cpp` |
| `STANLI_STRUCTURED_FRAMES=0` / `=1` | disable frames / force them on eligible plans | `structured_loop.cpp` |
| `STANLI_NO_STRUCTURED_FRAME_INSTANCE=1` | record every frame instead of building repeated frames from a template | `structured_loop.cpp` |
| `STANLI_STRUCTURED_CHECK_FRAME_INSTANCE=1` | also record each instantiated frame and require the same frame and recorder state | `structured_loop.cpp` |
| `STANLI_STRUCTURED_COMPILED_RECORDING=0` | record with the tree walk instead of the temporary program | `structured_instance_run.inc` |
| `STANLI_NO_DATA_LOOP_MEMO=1` | do not compile memoization for data-controlled loops | `structured_recording.inc` |
| `STANLI_DATA_LOOP_MEMO_MIN_WORK=n` | minimum executed steps before a data-loop memo is kept (default 256) | `structured_recording.inc` |
| `STANLI_DATA_LOOP_MEMO_MIN_RATIO=n` | minimum executed steps per recorded event for a kept memo (default 8) | `structured_recording.inc` |
| `STANLI_STRUCTURED_CHECK_REMAP=1` | check that no recorded pointer still refers to a released arena block | `structured_loop.cpp` |
| `STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1` | print selection, recording and frame diagnostics to stderr (also enables the remap check) | `structured_loop.cpp`, `lower_structured_loop.cpp`, `lower_stmt.cpp` |
| `STANLI_NO_REGION_RANGE_CACHE=1` | do not cache the range of an integer index expression over its loop-variable grid | `lower_structured_loop.cpp` |
| `STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS=1` | pass indexed region inputs through the packed layout instead of directly | `lower_structured_loop.cpp` |

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
than the unrolled loop (79-80% of the plain loop's time for the unroll
against 85-95% for a musttail chain). After these changes per-op cost is dominated by loading
the context, not the dispatch, so the remaining lever is fewer ops,
which is what the passes above are.

## Gather backward run compression (`elementwise.cpp`, reassociation, 10 ULP budget)

`OP_GATHER`'s backward scatter-adds `out_adj[k]` into `in_adj[idata[k]]` one
element at a time. Re-profiled 2026-09-04 on three gather-heavy radon models,
it was 47.0% of `radon_county_intercept`'s gradient (43.8% of total time),
39.6% of `radon_hierarchical_intercept_noncentered`'s, and 39.9% of
`radon_hierarchical_intercept_centered`'s, almost all in the backward half.
The kernel now scans for runs of consecutive equal indices (`county_idx`-style
data is grouped, not necessarily fully sorted) and accumulates each run into a
local before one store, cutting memory read-modify-writes to `in_adj` from one
per element to one per run. Indices that never repeat consecutively fall back
to the original per-element behavior bit for bit.

Measured on `radon_county_intercept`: `OP_GATHER` backward went from
79,546,392 ns to 11,322,356 ns per 2000-gradient batch (7.0x), taking the
opcode's share of the gradient from 47.0% to 16.3% and the per-gradient median
from 96,152.6 ns to 55,009.8 ns (1.75x). The other two models' `OP_GATHER`
share fell from 39.6-39.9% to 12.7%. Drift is reassociation-class, bounded at
10 ULP; the full corpus stayed at 130/130 models within 1e-09 of CmdStan.

## Unblocked: reduction reassociation (surveyed 2026-08-25, unblocked 2026-09-04)

This section is a backlog of candidates and measured dead ends. It does not
describe the current code. The candidates below change summation order
rather than any elementwise result. Each was measured or profiled during the 2026-08-25 precision survey
and is unblocked under the 10 ULP budget for reassociation-class changes.
Implementations must state that budget in the commit message and update
[`TESTING.md`](../../TESTING.md#every-model) and the conformance
baseline in the same commit. Re-profile before implementing any of them, since
profile share is a ceiling on the win and the `pow` entry below shows how a
large share can still yield nothing.

Softmax backward as a packet dot product (`adjoint.cpp`). Declined
2026-09-04. The 37% share once measured on `gpcm_latent_reg_irt` predates the
`OP_CATEGORICAL_LOGIT_GLM_LPMF` fusion; no corpus model now reaches a softmax
backward at gradient time. The reassociated dot also fails the 10 ULP budget
by construction: `adj_i = p_i (oa_i - d)` cancels when `oa_i` is close to
`d`, so a 1e-16 relative change in `d` became 23 to 128 ULP in the tests.

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
`CMakeLists.txt` stays. The packet-math fast forms (constrain,
`inv_logit`, `seq_sum`) measured 0.995 geomean: neutral, so they were removed.
And Eigen's vectorized `pow` is 5x slower than scalar `std::pow` on Apple
libm, with the `exp(k log x)` form costing 8 ULP, so the `POW` share of the
`dogs` models stays where it is.

## Constant folding and CSE on write_array (`lower.cpp`)

`run_write_array` used to stop after inplace, store-forward, and reroll, so
every data-only chain and duplicated GLM term from generated quantities
stayed in the graph (`covid19imperial_v2`: 595,563 ops). It now runs
constfold and CSE as well: 376,848 ops, per-draw write_array 7.22 ms to
3.37 ms; `hmm_gaussian` 1.20 ms to 0.61 ms. Compile cost is about 95 ms on
`covid19imperial_v2`, paid back within three draws. 29 of 120 corpus models
shrink, none grow, and the replay is unchanged. Partition, elide_stores, and
islands stay off: write_array has no target reduction to feed them.

## Data JSON parsing (`data.cpp`), declined 2026-09-04

`nn_rbm1bJ100` spends 1.95 s of its 2.8 s preparation in `json::parse` of a
183 MB `mnist.json` (94 MB/s, which is nlohmann's DOM speed; the copy into
`DataMap` is under 5%). A SAX walk would save at most half of that on the one
corpus model with data this size and costs a second numeric parser, so the
DOM parse stays.

## Removed after measuring flat

Two earlier optimizations were deleted in October 2026 because a paired
timing of the 340-model corpus found no gain. A fixed-size three-lane softmax
inside islands gave a geometric-mean speedup of 1.002x, and `iohmm_reg` was
4% slower without it (327 us against 340 us). Emitting reroll's vector
kernels directly for a terminal scalar loop, without expanding its iterations,
gave 1.0003x on gradients, and total corpus preparation time was 9.31 s with
it and 9.36 s without. Accuracy replay was unchanged by either.

## How we check all of this

Every pass changes the graph, and a wrong graph produces wrong numbers
silently. Three layers:

- Unit tests per pass (`tests/test_reroll.cpp`, `test_inplace.cpp`,
  `test_island.cpp`, `test_ode_prog.cpp`, `test_pass_safety.cpp`),
  including tests that the passes refuse what they must refuse, and a
  fuzz test that runs hundreds of random graphs through all passes and
  compares gradients before and after.
- The cross-path matrix (`tests/cross_path.hpp`), which CTest runs over
  every fixture and the cogmod models: each pass and path switch off or
  forced, the log density and every gradient component compared bitwise
  with the shipped pipeline at three points. After a change to a pass, run
  it over posteriordb too, with a scaled-error tolerance because the passes
  reassociate:
  `python3 -c 'import sys,tempfile,subprocess; sys.path.insert(0,"tools"); from corpus_inventory import corpus_cases, materialize_data; t=tempfile.mkdtemp(); [subprocess.run(["build-rel/stanli_check",str(c.source),str(materialize_data(c,t)),"--cross","--cross-rel-tol","1e-11","--cross-corpus"]) for c in corpus_cases("deps/posteriordb","posteriordb").values()]' | grep -E '^CROSS (FAIL|SKIP)'`.
  `--cross-rel-tol` passes a finite mismatch when |a - b| / max(|a|, 1e-300)
  is at most the bound. `--cross-corpus` adds stanli_check's default point
  and a config with rerolling, in-place, constant folding and islands off
  together. Today `sir` is skipped (out of support at every draw) and
  `ldaK5` fails at the default point under both pass-off configs, by up to
  1.25e-9 in `grad[3198]`: that entry is 1.8e-6 against gradient entries up
  to 180, and its absolute error of 2.2e-15 is reassociation error left
  behind by cancellation. Without the tolerance 45 of the 120 models report
  bitwise divergences, the largest elsewhere 8.7e-13.
- Direct verification against CmdStan (`tools/verify_sample.py`) for
  models a change affects.

Some of the env switches (`STANLI_NO_DATA_PRELOAD`, `STANLI_NO_INPLACE`,
`STANLI_NO_CONSTFOLD`, `STANLI_NO_REROLL`, `STANLI_NO_ISLAND`,
`STANLI_NO_ISLAND_COMPACT`, `STANLI_NO_ODE_DIRECT_RK`, and the others in
[Developer switches](#developer-switches)) exist so a wrong result
can be attributed to one optimization quickly, and they are how each one is
measured: every speed number is the same build with one variable set and
unset.

## Developer switches

Every `STANLI_*` variable that the runtime sources read is listed here, with
what it does and the file that reads it. The retained-loop switches are in the
[table in that section](#retained-loops). Unless a row says otherwise, a
switch acts when the variable is set to any value, including an empty value or
`0`, and the developer switches work only in a build configured with
`-DSTANLI_DEBUG_SWITCHES=ON` (see the top of this file). Three variables are
read with `getenv` directly and work in every build: `STANLI_NO_INTERPRETER`,
`STANLI_NO_FUSED_DENSITY` and `STANLI_ISA`.

Graph passes:

| switch | effect | read in |
| --- | --- | --- |
| `STANLI_NO_DATA_PRELOAD` | replay the generated data hydration through the interpreter | `lower.cpp` |
| `STANLI_NO_INPLACE` | turn off in-place updates, store forwarding and the eliding of full-extent stores | `inplace.cpp` |
| `STANLI_NO_CONSTFOLD` | turn off constant folding; acts only when the value starts with `1` | `constfold.cpp` |
| `STANLI_NO_REROLL` | turn off loop re-rolling, including the fixed-width row reduction pre-pass | `reroll.cpp` |
| `STANLI_NO_COLLAPSE` | turn off observation collapse in fast mode | `collapse.cpp` |
| `STANLI_COLLAPSE_REPORT` | print each term that collapse examined, with its observations, rows, groups and the form chosen or the reason for refusal | `lower.cpp` |
| `STANLI_NO_PARTITION` | turn off lane partitioning | `partition.cpp` |
| `STANLI_LANE_REPORT` | print why re-rolling and partitioning left each group of matching lanes unfused, and how many target terms were listed twice | `lower.cpp`, `pass_util.hpp` |
| `STANLI_NO_CSE` | turn off common-subexpression elimination | `cse.cpp` |
| `STANLI_NO_DENSITY_MASK` | bind every density argument in a generated backward as a recorder scalar again, instead of by its activity mask | `adjoint.cpp` |
| `STANLI_NO_FUSED_DENSITY` | use Stan Math's kernels instead of the fused kernels for the seven fused densities; acts when the value is set, nonempty and not `0`; ignored, with a notice, in a build with CMake option `STANLI_STAN_DENSITY_ORACLE=OFF` | `density_fused.cpp` |

Islands and register programs:

| switch | effect | read in |
| --- | --- | --- |
| `STANLI_NO_ISLAND` | turn off the island carver | `island.cpp` |
| `STANLI_NO_ISLAND_COMPACT` | turn off register-program compaction for islands | `island.cpp` |
| `STANLI_NO_ISLAND_LIVENESS` | turn off the liveness search that splits a run of ops into several islands | `island.cpp` |
| `STANLI_NO_ISLAND_JOIN_GUARD` | turn off the cost-floor shortcuts that decide between one joined island and several separate ones without compiling both | `island.cpp` |
| `STANLI_NO_ISLAND_PRICING_CACHE` | do not cache the prices of candidate splits, which are a speed-up of the search | `island.cpp` |
| `STANLI_ISLAND_PRICING_BUDGET` | unset or `1`: bound speculative pricing to the larger of 65,536 and four times the span; any other value removes the bound | `island.cpp` |
| `STANLI_ISLAND_ALWAYS` | skip the cost estimate and carve every run that compiles | `island.cpp` |
| `STANLI_DEBUG_ISLAND` | print the carver's decisions and cost estimates | `island.cpp` |
| `STANLI_NO_NATIVE_ADJ` | replay islands and regions under stan-math's autodiff instead of running a generated backward | `island.cpp`, `lower_stmt.cpp` |
| `STANLI_NO_PROGRAM_DEST_FORWARD` | do not redirect a producer to write the destination of the copy that follows it | `program.cpp`, `island.cpp` |
| `STANLI_NO_FILL_SINK` | do not delay a constant fill until a path reads it | `program.cpp` |
| `STANLI_NO_CFG_DEAD_CONSTANTS` | do not remove unreachable instructions and constant stores that are overwritten before any read | `program.cpp` |
| `STANLI_NO_COPY_PROP` | turn off copy propagation in register programs | `program.cpp` |
| `STANLI_NO_NATIVE_MATH` | compile `erfc`, `log1p`, `log1m_exp` and `inv_square` as calls to the graph's kernels, with no register-program instruction of their own | `mir_prog.hpp` |
| `STANLI_NO_REGION_DIRECT_OUTPUT` | extract a region's only live-out through an op instead of reading the region's output slot directly | `lower_stmt.cpp` |
| `STANLI_NO_REPLAY_REUSE` | do not reuse the register workspace of an island that replays under autodiff | `kernels/island.cpp` |
| `STANLI_NO_ODE_DIRECT_RK` | solve ODEs through Stan Math's autodiff solve instead of the direct RK sensitivities | `lower_higher_order.cpp` |

Region map:

| switch | effect | read in |
| --- | --- | --- |
| `STANLI_REGION_MAP=0` | never map a loop; `all` also maps loops that do not branch on a parameter (for measurement); any other value keeps the default | `lower_region_map.cpp` |
| `STANLI_REGION_MAP_DIAGNOSTICS` | print why each loop was refused or mapped, and the form it got | `lower_region_map.cpp` |
| `STANLI_REGION_MAP_LANES=0` | skip the lane form | `lower_region_map.cpp` |
| `STANLI_REGION_MAP_SAVE_LIMIT=n` | the limit on saved cells for all iterations, default 2^24 | `lower_region_map.cpp` |
| `STANLI_REGION_MAP_CHECK_CLEAN` | check after each iteration or tile that the adjoint cells are zero again, and throw if not | `lower_region_map.cpp` |
| `STANLI_REGION_MAP_TILE_CALLS=0` | turn off the tiled form of kernel calls in the lane form | `region_map_lanes.cpp` |
| `STANLI_REGION_MAP_TILE_CELLS=n` | recompute each tile's forward registers in the backward when it needs more than `n` registers, instead of saving every tile's | `region_map_lanes.cpp` |
| `STANLI_REGION_MAP_PROFILE` | print occupancy and per-opcode counts for the lane form when the process exits | `region_map_lanes.cpp` |
| `STANLI_PROGRAM_BUDGET`, `STANLI_LOOP_BUDGET`, `STANLI_UNROLL_CEILING` | the program budget, the size past which a loop may become `OP_LOOP`, and the most ops one loop may unroll to (see [Keeping each chain's program small](#keeping-each-chains-program-small-lowercpp-disable-stanli_program_budget0)) | `lower.cpp` |

Lowering policy and write_array:

| switch | effect | read in |
| --- | --- | --- |
| `STANLI_BOUNDED_SPECIALIZATION` | unset or `1`: for a model with a loop whose branch or `while` condition is not data-only, first try a second lowering with retained loops turned off, and use it when it succeeds; any other value skips the attempt | `lower.cpp` |
| `STANLI_PARALLEL_WA` | lower `write_array` beside the log density: `auto` or empty is the default, `1` or `force` always uses a second thread, any other value lowers them one after the other; `CompileOptions::parallel_write_array` does this in every build | `lower.cpp` |
| `STANLI_SHARE_WRITE_ARRAY` | with several chains, `1` shares one `write_array` graph's storage across them and any other value copies it; unset shares when copying would cost 1 GiB or more | `executor.cpp` |
| `STANLI_WA_FORCE_INTERP` | attach the per-draw interpreter to a model whose graph covers `write_array`; for the cross-path harness only, because drivers prefer the interpreter when it is there | `lower.cpp` |
| `STANLI_NO_INTERPRETER` | refuse a model that has a part without a compiled path; acts when the value is nonempty and not `0` | `lower.cpp` |
| `STANLI_ISA` | `avx2` runs the AVX2 copies of the matrix kernels on x86-64; unset uses the baseline copies | `isa_dispatch.cpp` |

Reports and dumps:

| switch | effect | read in |
| --- | --- | --- |
| `STANLI_DUMP_PASSES` | write one graph dump per lowering stage, to a directory or, with `-`, to stdout; `CompileOptions::dump_passes` does this in every build | `lower.cpp` |
| `STANLI_DUMP_STAGES` | choose the stages to dump, by name or as `graph:stage`; `CompileOptions::dump_stages` does this in every build | `lower.cpp` |
| `STANLI_PROFILE_PREP` | print preparation time for each stage; acts when the first character is not `0` | `lower.cpp`, `lower_structured_loop.cpp` |
| `STANLI_EXECUTION_REPORT` | print which engine each part of the model runs on; acts when the value is nonempty and not `0`; `CompileOptions::execution_report` does this in every build | `execution_report.cpp` |

## Historical targeted measurements

This appendix preserves the optimization-specific material that originally
lived in `docs/benchmarks.md`. These are dated, targeted A/B medians used to
attribute a change to one mechanism; they are not replacements for the current
absolute corpus results in [`docs/benchmarks.md`](../../docs/benchmarks.md).
Some results repeat measurements alongside the implementation sections above
so that none of the historical performance record is lost. Each entry describes
the code at its own date, so the eligibility rules in it can be narrower than
the ones in the sections above, which describe the current code. The
`STANLI_PROFILE=1` switch that some entries use no longer exists. The nearest
current switches are `STANLI_PROFILE_PREP` and `STANLI_EXECUTION_REPORT`.

### Kernel-tail attribution

A profile of the models that were below parity at the time
(`STANLI_PROFILE=1`) showed that the tail was mostly not a graph problem: in
seven of them, one precompiled kernel accounted for half to nine-tenths of the
gradient. `diamonds` was the extreme, with 90.9% in one GLM kernel that rebuilt
a var tape in both sweeps. Differentiating once and stashing the partials took
it from 0.48x to 0.89x CmdStan in the targeted A/B; its current warmed mean is
1.01x. `prophet` spent 82% in `OP_MATVEC` with one serial accumulation chain.
Four independent accumulators moved it from 0.67x to 1.23x in the targeted
A/B, bitwise unchanged; its current corpus result is 1.28x.

### Where the wins came from

The interpreter's cost is per op, not per element: ~17-20 ns for a
scalar density op forward + backward, against ~0.9 ns for the actual
math (`tools/bench_opcost.cpp`). A vectorized statement over N elements
amortizes that to nothing and runs precompiled stan-math on contiguous
doubles, while CmdStan pays its var-tape cost per scalar per
evaluation. Vectorized models therefore win, and models stuck as
unrolled scalar loops used to lose (0.4-0.9x) until the graph passes
closed the gap. The passes themselves are described above; the
headline historical measurements follow:

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
- **Lane partitioning and CSE** (repeated work re-rolling cannot reach):
  segmenting the graph at target terms and element stores finds lanes that are
  neither adjacent nor in phase, and value numbering removes the terms an
  unrolled model emits twice.
  `state_space_stochastic_level_stochastic_seasonal` goes 1,375 ops -> 19
  (2.29x), `Mth_model` 1,563 -> 35 (1.63x), `Mh_model` 1,542 -> 18 (1.53x),
  `Survey_model` 1,427 -> 9, and `Mt_model` 1,062 -> 70 under CSE alone
  (16.1x). The IRT arm rewrites `gpcm_latent_reg_irt`'s per-item chain as
  `categorical_logit_glm_lpmf`, 34,634 ops -> 91 (6.5x internally).
- **Post-reroll in-place slices** (chained partial matrix fills):
  `Mtbh_model` keeps the same 1,585-op graph, but 146 stores now move four
  values rather than copying a 730-value matrix. Median gradient latency
  falls 106.5 -> 47.4 us (2.24x); direct replay remains within 3.61e-15 of
  the CmdStan references across 465 values.
- **Native Bernoulli forwards** (recorder-bound scalar and short-vector
  calls): both Bernoulli parameterizations have one real argument and one
  analytic partial, so they write that column directly into the density
  scratch instead of constructing the generic recorder edge. Summed vector
  logits preserve Eigen's packet `exp`, select, and reduction order while
  reusing the partial column as their `ntheta` workspace. `Mt_model` falls
  30.6 -> 19.2 us, `Mth_model` 113.2 -> 57.3 us, and `Mtbh_model` 47.4 ->
  26.8 us after the slice fix. The combined `Mtbh_model` improvement is
  106.5 -> 26.8 us (3.98x). Common-subexpression elimination and lane
  partitioning have since taken the same three models further: their
  warmed-mean rows are now 4.47, 23.4, and 12.9 us/gradient, or 4.47x, 4.01x,
  and 3.32x CmdStan.
- **Native scalar probability categorical** removes the nested autodiff replay
  only when one categorical outcome selects from an active probability vector.
  Stan Math's double overload still computes the value and performs every
  check; reverse adds the incoming seed divided by the selected probability to
  that probability's adjoint. Array outcomes retain replay to preserve their
  repeated-selection accumulation topology, and categorical-logit calls retain
  replay for their dense pullback. The graph is unchanged. In a targeted
  2026-08-24 Release A/B (seven matched-run medians),
  `gpcm_latent_reg_irt` moved 1.741465 -> 0.955609 ms/gradient (1.8224x
  internally, 1.3998x CmdStan at that point), and `grsm_latent_reg_irt` moved
  0.9705208 -> 0.4953192 ms/gradient (1.9594x internally, 1.5387x
  CmdStan). Their categorical opcode time fell 4.45x and 5.00x respectively;
  the categorical-logit RBM controls were unchanged. Lane partitioning later
  replaced those per-item categorical calls with one
  `categorical_logit_glm_lpmf` each, so the current full-corpus warmed means in
  [`docs/benchmarks.md`](../../docs/benchmarks.md) are 121.1 us (11.0x CmdStan)
  and 70.9 us (10.8x).
- **Compiled scalar generated-quantities RNGs** keep caller-owned chain state
  on the forward-only write-array graph for scalar `poisson_log`, `uniform`,
  `bernoulli`, `normal`, `lognormal`, and `binomial` draws.
  Apart from the audited categorical and multivariate-normal extensions below,
  other RNGs, container-valued results, and draws used as dynamic control,
  indices, or geometry still fail closed to the whole-section interpreter. In
  an exact census of the 24 previously
  interpreted corpus models, this RNG tranche moved 12 to the graph; all 24
  still produced complete rows.
  A targeted 2026-08-24 C-ABI A/B (point 0, two warmups, seven matched batch
  medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `covid19imperial_v2` | 156.239 ms | 2.089 ms | 74.81x |
  | `covid19imperial_v3` | 157.176 ms | 2.077 ms | 75.69x |
  | `dogs_hierarchical` | 1.929 ms | 24.105 us | 80.02x |
  | `dogs_nonhierarchical` | 2.078 ms | 32.317 us | 64.29x |
  | `GLMM1_model` | 75.541 us | 2.176 us | 34.72x |
  | `hierarchical_gp` | 2.291 ms | 46.301 us | 49.47x |
  | `lotka_volterra` | 1.797 ms | 31.434 us | 57.16x |
  | `one_comp_mm_elim_abs` | 5.123 ms | 139.431 us | 36.74x |
  | `M0_model` | 15.378 us | 0.119 us | 129.55x |
  | `Mb_model` | 2.020 ms | 26.703 us | 75.65x |
  | `Rate_4_model` | 4.007 us | 0.126 us | 31.93x |
  | `Rate_5_model` | 4.311 us | 0.124 us | 34.82x |

  The largest setup tradeoff is the two Covid graphs: C-API model
  construction rises from about 0.239 s to 2.20-2.21 s, but the 154 ms saved
  per row repays it after roughly 13 draws. For the four added scalar-binomial
  models, across 1,000 rows of each model their aggregate time falls from
  2.0438 s to 0.0271 s (75.50x), and construction also gets faster in every
  case, so break-even is immediate. Their graph and frozen-interpreter rows
  were bitwise identical for all 28,926 compared values. These are targeted
  write-array medians, not replacements for the sampling columns in the full
  corpus table.
- **Compiled generated-quantities reductions** add an exact forward-only
  product for the vector/row-vector surfaces used by the capture-recapture
  models, plus integer sum only when a one-dimensional runtime array is proved
  fully initialized, integral, and safe from 32-bit overflow. Product grouping
  follows Stan Math's expression provenance: materialized vectors use its
  address-independent packet grouping, while strided matrix-row expressions
  retain ascending scalar grouping. Shifted views, arbitrary expressions,
  reverse-mode products, and unproved integer arrays still fail closed to the
  interpreter. This moved `Mh_model`, `Mt_model`, `Mtbh_model`, and `Mth_model`
  to the graph, taking the then-current 24-model census from 12 graph / 12
  interpreter to 16 / 8. The categorical tranche below subsequently advances
  that census to 17 / 7, the extrema tranche advances it to 18 / 6, and the
  multivariate-normal tranche advanced it to 19 / 5, and the runtime-control
  tranche below completes it at 24 / 0. All 119 compiling corpus models still
  produce complete rows. A targeted 2026-08-24
  C-ABI A/B (point 0, two warmups, seven matched
  batch medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `Mh_model` | 869.179 us | 12.494 us | 69.57x |
  | `Mt_model` | 20.867 us | 0.127 us | 164.83x |
  | `Mtbh_model` | 1.046 ms | 9.856 us | 106.18x |
  | `Mth_model` | 1.099 ms | 18.205 us | 60.36x |

  Across 1,000 rows of each model, aggregate row time fell from 3.035338 s to
  0.040682 s (74.61x). Including one construction of each model, it fell from
  3.056579 s to 0.075491 s (40.49x); the equal-mix aggregate setup cost breaks
  even after five rows per model. Within a 146,196-value comparison, every
  product-fed draw and final integer sum matched the frozen interpreter
  bitwise, including a fourth-row stream-continuation check. `Mtbh_model` and
  `Mth_model` also expose the pre-existing graph/interpreter boundary in
  deterministic transformed parameters: their `p` columns differ by at most
  two ULP, while the graph is closer to live CmdStan at the shared point. These
  are targeted write-array medians; the full-corpus sampling table awaited the
  next refresh.
- **Compiled categorical generated-quantities RNGs** extend the same
  scalar-result `OP_RNG` path to `categorical_rng(vector)`. The graph and
  `WaInterp` each copy their materialized probability vector into one shared
  helper and call the pinned Stan Math implementation, preserving its simplex
  validation order, one-based int result, and exact caller-owned stream
  consumption. Lowering marks the int initialized but deliberately does not
  infer a `[1, K]` range, so a later dynamic index still selects the
  whole-section interpreter.

  In a targeted matched C-ABI A/B, `Survey_model` moved from 1011.4736 to
  60.3127 us/row (16.7705x), saving 0.9511609 s per 1,000 rows. Construction
  moved from 4231.333 to 5406.709 us, so the 1175.376 us setup delta amortizes
  after 1.236 rows, or two whole rows. It was the only model to change in the
  exact 24-model write-array census, moving graph/interpreter coverage from
  16 / 8 to 17 / 7; all 24 census models and all 119 compiling corpus models
  still produced complete rows. At that stage, `iohmm_reg` remained
  interpreted at the later dynamic-index barrier, `value must be known at
  compile time: hatz`; the runtime-control tranche below now compiles that
  index and its subsequent Viterbi decoder.

  The categorical draw `n` matched exactly in all 18/18 C-ABI comparisons:
  seeds 0, 1, 2, 7, 1234, and `UINT32_MAX`, each continued for three sequential
  rows.
  The same full-row comparison recorded 8,532 expected bit differences in
  deterministic columns from the pre-existing graph/interpreter numerical
  boundary; they are not RNG mismatches. Focused tests avoid that boundary by
  routing the identical probability vector through the shared helper and the
  direct pinned Stan Math call, then comparing the next engine state. These
  targeted results do not refresh `docs/internal/artifacts/corpus-bench.tsv` or the current
  [full-corpus table](../../docs/benchmarks.md#full-corpus).
- **Compiled generated-quantities extrema** add the
  `OP_EXTREMA_VEC` min/max variants. Lowering admits only a top-level
  write-array call on a direct `UVector` or `URowVector` `Var` and evaluates
  it with the same address-independent grouping that pinned Stan Math uses for
  an owning Eigen value. Empty-real results are preserved: `min` is positive
  infinity and `max` is negative infinity. Arrays, matrices, indexed views,
  expressions, and UDF calls still select `WaInterp`.
  The opcode is excluded from reroll matching and interpreter islands by its
  `kVariantGrouped` trait.

  `losscurve_sislob` was the only model to change in the exact 24-model
  census, moving graph/interpreter coverage from 17 / 7 to the then-current
  18 / 6; the multivariate-normal tranche below advanced it to 19 / 5, and the
  runtime-control tranche completes it at 24 / 0. All 24 census models and all
  119 compiling corpus models retained complete rows. Its 1,218-op graph
  contains exactly one length-10 min opcode
  and one length-10 max opcode, and writes 384 columns. Graph and `WaInterp` matched
  bitwise for all 1,536/1,536 compared values; the same-input pinned Stan Math
  check matched 8/8 cases.
  Against 1,200 stored CmdStan values, the worst difference was 4.44e-16, or
  eight ULP.

  In a targeted matched C-ABI A/B, `losscurve_sislob` moved from 329.9520 to
  3.3704 us/row (97.8970x), saving 0.3265816 s per 1,000 rows. Construction
  moved from 4107.375 to 5291.959 us, a 1184.584 us setup increase that
  amortizes after 3.627 rows, or four whole rows. These targeted results do not
  refresh `docs/internal/artifacts/corpus-bench.tsv` or the current
  [full-corpus table](../../docs/benchmarks.md#full-corpus).
- **Compiled covariance-form multivariate-normal RNGs** extend `OP_RNG` to
  the audited `multi_normal_rng(vector, matrix) -> vector` write-array surface.
  The mean length and square covariance shape are fixed by lowering, and both
  the graph and `WaInterp` copy their column-major inputs into the same owning
  Eigen values before calling pinned Stan Math. This preserves its finite,
  symmetry, and positive-definiteness validation order and its exact normal
  draw schedule. Array overloads, non-square or mismatched shapes, and
  rectangular Cholesky forms remain on the whole-section interpreter.
  The [RNG follow-up](../../notes/execution/2026-09-28-vector-rng-implementation.md)
  adds square single-vector Cholesky, Poisson-binomial, and categorical
  register-region coverage. It also documents an explicit empty-logit guard
  around upstream categorical-logit RNG, whose empty case has an unchecked read.

  `multi_occupancy` was the only model to change in the exact 24-model census,
  moving graph/interpreter coverage from 18 / 6 to the then-current 19 / 5.
  The runtime-control tranche below subsequently completes the census at
  24 / 0 and makes all 119 compiling corpus write arrays graph-backed. All
  rows remain complete. Its
  graph and forced-interpreter rows were bitwise identical for all 5,616
  values from six seeds continued for three sequential rows.

  In a targeted 2026-08-25 matched C-ABI A/B (point 0, two warmups, seven
  batch medians), `multi_occupancy` moved from 298.9260 to 5.4898 us/row
  (54.4512x), saving 0.2934362 s per 1,000 rows. Construction also improved,
  from 5864.792 to 5368.583 us, so there is no setup break-even penalty. These
  targeted results do not refresh `docs/internal/artifacts/corpus-bench.tsv` or the current
  [full-corpus table](../../docs/benchmarks.md#full-corpus).
- **Compiled generated-quantities runtime control** completes the last five
  interpreted write arrays: `hmm_drive_0`, `hmm_drive_1`, `hmm_example`,
  `hmm_gaussian`, and `iohmm_reg`. Their shapes and loop bounds are static, but
  their branches and Viterbi backtracking indices depend on the current draw.
  Lowering now places each enclosing block in one structured register program,
  with checked one-level dynamic indexing and packed live-ins when the block
  needs more than six logical inputs. The historical 24-model census moves
  from 19 graph / 5 interpreter to 24 / 0; all 119 compiling corpus models now
  have complete, graph-backed write arrays.

  A targeted 2026-08-25 matched C-ABI A/B (point 0, two warmups, seven batch
  medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `hmm_drive_0` | 6.777 ms | 41.975 us | 161.45x |
  | `hmm_drive_1` | 7.396 ms | 44.426 us | 166.48x |
  | `hmm_example` | 1.388 ms | 9.718 us | 142.85x |
  | `hmm_gaussian` | 37.366 ms | 649.125 us | 57.56x |
  | `iohmm_reg` | 38.642 ms | 713.011 us | 54.20x |

  Across 1,000 rows of each model, aggregate row time fell from 91.568918 s
  to 1.458255 s (62.79x). Including one construction of each model, it fell
  from 91.989657 s to 1.990729 s (46.21x); the aggregate setup increase pays
  back after two whole rows per model. The four HMMs matched 51,696 checked
  Viterbi outputs bitwise across four points, three seeds, and three sequential
  rows. In `iohmm_reg`, all categorical and Viterbi states/scores matched; the
  only differences were 2,945 continuous simulations inheriting the
  pre-existing transformed-input boundary, bounded by 8.89e-16. Exact later
  state draws confirm stream alignment. These targeted results do not refresh
  `docs/internal/artifacts/corpus-bench.tsv` or the current
  [full-corpus table](../../docs/benchmarks.md#full-corpus).
- **Compiled scalar integer RNG control** extends the register-machine path
  to generated-quantity branches, rejection loops and checked indices driven
  by integer draws. Effectful integer initializers cannot run during lexical
  constant discovery; draw-dependent extents still refuse to the interpreter.
  Poisson, Student-t and Bernoulli-logit use the shared Stan Math draw kernel.
  Tests compare repeated graph/interpreter/copy outputs and subsequent RNG
  state bitwise, including empty loops, dynamic-size fallback and invalid
  indices. In the 2026-09-14 educational revision A/B, together with compiler
  procedure pruning and buffered 17-digit CLI formatting, hurdle-Poisson
  improved 4.75x, simple Poisson 8.51x and Student-t regression 1.96x end to
  end. All 52 complete CSVs (13 models, four seeds) matched the old binary
  byte for byte. All 13 models clear the 0.5x vectorized CmdStan wall-time
  floor, including Stanli source compilation. See the
  [full educational results](../../tests/educational/RESULTS.md) for the
  preparation tradeoff, phase measurements and raw observations.
- **Allocation-free ODE right-hand-side input seeding** removes the promoted
  `y` and `theta` staging vectors built on every solver callback and seeds the
  reusable register file directly. A targeted 2026-08-24 Release A/B (seven
  alternating matched-batch medians) measured:

  | model | staged inputs | direct register seeding | improvement |
  | --- | ---: | ---: | ---: |
  | `lotka_volterra` | 78.6216 us | 68.6068 us | 1.14597x |
  | `soil_incubation` | 102.1527 us | 89.5593 us | 1.14062x |
  | `one_comp_mm_elim_abs` | 643.1571 us | 578.8621 us | 1.11107x |

  The geometric-mean improvement is 1.13245x, and the known callback counts
  put the saving at a consistent 38.5-39.3 ns/callback. LP and gradient
  results were bitwise identical for all 63/63 checked scalars across three
  points. This targeted A/B attributes the direct-seeding change; the current
  corpus rows in [`docs/benchmarks.md`](../../docs/benchmarks.md) are the later
  end-to-end refresh.
- **Direct ODE Jacobian-row harvest plus scalar RHS cleanup.** Stan Math's ODE
  outputs are already precomputed-gradient nodes directly connected to active
  inputs, so the kernel now chains the selected output instead of running a
  nested reverse sweep across every sibling output for each Jacobian row, and
  clears only that output and its inputs between rows. A conservative
  load-time pass also removes overwritten scalar initializer
  constants and aliases stable single-definition copies; any range-reading,
  dynamic, density, or kernel instruction leaves the RHS program unchanged. A
  targeted 2026-08-25 three-binary A/B (seven rotating matched-batch medians)
  measured:

  | model | parent | direct node only | full change | improvement |
  | --- | ---: | ---: | ---: | ---: |
  | `lotka_volterra` | 78.5160 us | 70.3923 us | 60.8881 us | 1.28951x |
  | `soil_incubation` | 101.3979 us | 88.5437 us | 82.0553 us | 1.23573x |
  | `one_comp_mm_elim_abs` | 663.9302 us | 665.3790 us | 629.7752 us | 1.05423x |

  The full change improves the geometric mean by 1.18876x. Fresh parent and
  patched checkers were byte-identical for all 63/63 LP-and-gradient scalars
  over three evaluation points. These targeted medians isolate the mechanism;
  they do not refresh the full-corpus table or its retained CmdStan columns.
- **Packed row-wise reductions** (the LDA inner loop): targeted medians fall
  from 154 to 94 us for `ldaK2` and 6.82 to 3.70 ms for `ldaK5`, while their
  graphs collapse from 15,854 to 22 and 434,126 to 156 ops. With the mixture
  kernels' analytic partials on top, the full warmed-mean rows are 48.2 us
  (2.16x) and 2.36 ms (2.36x).
- **Native symmetric-eigen pullbacks** remove reverse-time eigensolves from
  `kronecker_gp`: the targeted median falls 289.0 -> 185.7 us/gradient. Its
  current warmed mean is 183.3 us, 1.19x CmdStan.
- **Native Cholesky-density partials** cover the exact single-observation,
  Cholesky-factor-active `multi_normal_cholesky` shape in `gp_regr`: the
  targeted median falls 6.05 -> 4.20 us/gradient. Its current warmed mean is
  4.04 us, 1.16x CmdStan.
- **Elementwise-lp fusion** (the mixture idiom): `low_dim_gauss_mix`
  7,208 ops -> 16, crossing parity (0.78x -> 1.07x) at the time;
  `normal_mixture` 13 ops. Analytic mixture partials took both further, to
  2.04x and 2.11x in the current run.

### Tape islands, measured

The island pass compiles irreducible scalar residue (recurrences) into
one register-machine op. For most of this pass's life the op collapse was
dramatic on every model with such a region and the time followed on
exactly one, because the backward re-executed the whole program under
`stan::math::var`: a vari allocated per operation, a virtual `chain()`
per operation, a nested tape built and torn down per call. That is
correct by construction and it costs what CmdStan costs, so the island
bought data movement and nothing else -- `iohmm_reg`, whose steps copy a
1,500-element state vector, won 2.5x, and the estimate refused nearly
everything else.

`gen_adjoint` (`runtime/src/adjoint.cpp`) generates the backward instead:
reverse-mode source transformation over the ~35 opcodes of `Program`,
producing a second pass over doubles with no vari, no nested tape and no
allocation. Measured with `STANLI_NO_ISLAND=1` as the baseline, same
build, same point, on all twenty-one corpus models that compile a
region (`harnesses/island_ab.py`, since removed; min of three runs each -- the sweep
bypasses the carve estimate so the regions it declines are measured
too, which is how the table can hold rows the default build refuses):

| model | ops off -> on | ns/grad off | replayed | generated | generated speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| `iohmm_reg` | 53,456 -> 27 | 1,428,776 | 574,888 (2.49x) | 301,629 | **4.74x** |
| `hmm_gaussian` | 42,926 -> 11 | 365,750 | 398,088 (0.92x) | 228,638 | **1.60x** |
| `hmm_example` | 3,483 -> 13 | 32,292 | 36,465 (0.89x) | 20,766 | **1.56x** |
| `hmm_drive_1` | 19,540 -> 24 | 171,084 | 199,678 (0.86x) | 121,344 | **1.41x** |
| `hmm_drive_0` | 19,540 -> 24 | 162,653 | 195,899 (0.83x) | 117,285 | **1.39x** |
| `garch11` | 1,797 -> 8 | 10,996 | 14,777 (0.74x) | 8,109 | **1.36x** |
| `Mb_model` | 7,035 -> 1,646 | 72,289 | 71,934 (1.00x) | 65,250 | **1.11x** |
| `arma11` | 1,205 -> 9 | 6,774 | 10,526 (0.64x) | 6,544 | 1.04x |
| `accel_gp` | 461 -> 64 | 7,233 | 8,170 (0.89x) | 7,041 | 1.03x |
| `losscurve_sislob` | 316 -> 26 | 2,340 | 3,165 (0.74x) | 2,281 | 1.03x |
| `multi_occupancy` | 4,006 -> 3,659 | 68,098 | 70,760 (0.96x) | 68,097 | 1.00x |
| `hier_2pl` | 349 -> 97 | 301,507 | 302,117 (1.00x) | 301,792 | 1.00x |
| `soil_incubation` | 129 -> 32 | 96,445 | 96,704 (1.00x) | 96,903 | 1.00x |
| `kronecker_gp` | 254 -> 166 | 302,779 | 315,098 (0.96x) | 306,665 | 0.99x |
| `accel_splines` | 425 -> 28 | 7,745 | 8,963 (0.86x) | 7,882 | 0.98x |
| `hierarchical_gp` | 165 -> 84 | 30,448 | 35,607 (0.86x) | 31,041 | 0.98x |
| `covid19imperial_v3` | 21,526 -> 19,995 | 308,879 | 439,340 (0.70x) | 316,736 | 0.98x |
| `covid19imperial_v2` | 21,526 -> 19,995 | 305,642 | 442,001 (0.69x) | 315,311 | 0.97x |
| `Survey_model` | 1,427 -> 5 | 61,524 | 62,030 (0.99x) | 65,039 | 0.95x |
| `dugongs_model` | 120 -> 12 | 768 | 766 (1.00x) | 1,168 | 0.66x |
| `bones_model` | 7,528 -> 4,955 | 52,335 | 988,998 (0.05x) | 207,721 | 0.25x |

Three of the twenty-one exist because the machine's vocabulary stopped
being a subset of the graph's: any scalar-out op it has no instruction
for now compiles as a CALL to the graph's own kernel -- the identical
code, partials, and backward the op would have run -- so one such op no
longer ends a run (`POW` used to split regions in half). A CALL buys
continuity, never speed, and the estimate charges it the graph's own
per-op tax; without that charge the first sweep carved `dugongs_model`
at a measured 0.66x and `Survey_model` at 0.95x, and with it both are
refused on the default path while every previously carved verdict is
unchanged. `losscurve_sislob` is the payoff shape: its residue drops
88 -> 26 ops because the cdfs inside it stopped ending the run.

Every region is faster generated than replayed, and the class changed
rather than improved: op collapse is now worth roughly what the op counts
always suggested it should be. `hmm_gaussian` collapses 42,926 ops to 11
and measured 0.92x replayed against 1.60x generated. The ceiling is
parity-plus and not more, as predicted before measuring -- CmdStan's
generated code is inlined compiled C++ and the adjoint program still pays
interpreted dispatch per instruction -- and `iohmm_reg` beats it only
because the registers also make its vector copies disappear.

The estimate changed with it. It weighed the register file 4x because the
file was built as vars, and that term is what refused thirteen of the
fourteen regions it could compile. A value register is a memory cost again:
one forward write and one backward read. The adjoint file is a separate
cost, however. Checkpoints hold values only, and registers made equivalent
by a copy already share an adjoint cell to reproduce stan-math's tape order.
Those equivalence classes are now packed densely, so the runtime zeroes one
cell per distinct class rather than one per value or checkpoint register.
The estimate mirrors the storage exactly: two passes over value and
checkpoint registers other than CALL scratch, one pass over the compact
adjoint file, both instruction streams, and the existing neutral charge for
CALLs.

What an island buys is still mostly the per-op tax the graph pays -- a
dispatch, a context load and a scratch-partials backward, ~5 ns against
~1 ns for an island instruction (`kOpCost = 5`). Without it a region like
`garch11`, whose scalar ops barely move more elements than there are ops,
reads as a wash. The 2026-08-24 structural census reran default, disabled,
and forced islands on all 21 corpus models with compilable regions. Compact
accounting changed exactly one decision, `iohmm_reg`, and preserved every
previous selection and refusal, including the measured loss guards
`bones_model`, `dugongs_model`, `Survey_model`, and both `covid19imperial`
models. `STANLI_ISLAND_ALWAYS=1` skips the estimate, which is how to ask why
a region was left alone.

In the targeted Release A/B from that census, `iohmm_reg`'s 95,424 forward
register ids reduce to 39,000 distinct adjoint cells; 4,488 additional
checkpoint registers remain value-only. Its estimated island cost falls
from 389,640 to 328,728 against the graph's 361,045, so the default path now
collapses 53,456 ops to 27. Seven clean, interleaved runs moved the median
from 498.612 to 241.453 us/gradient (2.065x internally), within 0.3% of the
forced-island path and 1.33x faster than the retained 320.335 us CmdStan
reference. This is a targeted A/B; at the time, the warmed corpus table was the
last full-corpus run pending the next benchmark refresh. The current absolute
corpus result remains in [`docs/benchmarks.md`](../../docs/benchmarks.md).

`STANLI_NO_NATIVE_ADJ=1` restores the replay. It changes nothing else --
the adjoint is still generated, the estimate still assumes it, and the
forward program is identical -- so the two backwards are compared over
the same islands, which is what the "replayed" column above is.

Fifteen of the eighteen agree with the replay **bitwise**; each rule is
the corresponding stan-math rev expression transcribed, grouping
included. The other three reassociate one sum: a var copy shares a vari,
so from the copy onward both registers accumulate into one adjoint, and
`gen_adjoint` shares an adjoint cell to match -- but a cell is shared for
the whole program where a vari is shared only until the destination is
next written. Where they differ the arbiter is the op graph the island
replaced, not the replay, and the generated adjoint is closer to it in
all three: `Mb_model` reproduces the op graph exactly where the replay
was 1.07e-14 away, and `iohmm_reg` is 3.46e-13 against the replay's
6.01e-13.

Beyond the estimate, islands still refuse propto densities (their
term-dropping depends on argument types, which the island's uniform
binding cannot reproduce), runs under 32 ops, and regions producing
target terms. Parameter-dependent forward branches can use generated
adjoints. Each block that produces an adjoint appends its segment index to a
trail as it starts (opcode PATH), and the reverse pass visits the recorded
segments from last to first, so the per-iteration cost follows the blocks
the path executed and not the number of blocks in the body. The lane plan
reads the same PATH instructions to build per-tile segment masks, and walks
segments in descending index order. Back edges and unsupported derivatives
retain the replay. The branch tests in
`tests/test_adjoint.cpp` cover reused frames, conditional overwrites, nested
branches, kernel calls, and refusal of uninitialized joins.

### ODE models and preparation

The before/after numbers in this section are targeted historical A/B medians;
the current warmed-mean corpus rows are called out separately.

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
the model's interpreter-fallback warning reports when that happens.

A later mixed-activity specialization also preserves Stan Math's separate
scalar types for the initial state and parameters. Previously either active
input promoted both to reverse mode and integrated sensitivities for both.
On `one_comp_mm_elim_abs` the initial state is data, so the sensitivity width
falls from four to three and median latency from 699 to 639 us/gradient. The
fully active `lotka_volterra` and `soil_incubation` shapes take the same path
as before and remain flat within measurement noise. After the callback and
Jacobian-harvest work described earlier in this appendix, the full-corpus
warmed means were 523 us for `one_comp_mm_elim_abs`, 47.6 us for
`lotka_volterra`, and 67.8 us for `soil_incubation`, or 0.90x, 0.87x, and
0.90x CmdStan. Current absolute values remain in
[`docs/benchmarks.md`](../../docs/benchmarks.md).

Preparation scales too: the largest corpus model (`nn_rbm1bJ100`, MNIST,
60,000 rows, 79,411 parameters) lowers to a 132,024-op graph. Its old 23.80 s
compile was almost entirely stanc's generated loop reconstructing the
47-million-element input matrix after `DataMap` had already parsed it. Direct
typed input preload removes that loop; an indexed reroll candidate scan removes
another empty 0.13 s pass over the resulting graph. Together they reduce graph
compilation to 0.23 s (103x), and the full MIR/data-to-bound-executor path from
26.33 s to 2.76 s (9.5x); JSON parsing is now the largest preparation stage.
The same profiled A/B removes 1.34 GB of peak RSS. The log density and all
79,411 gradient components remain within the existing CmdStan oracle
tolerance. Overall, graph
compilation is 4-400 ms against a 6.2-7.6 s CmdStan compile (warm precompiled
header, after a multi-minute one-time `make build`); that gap is what
time-to-first-draw is made of. The current corpus run records 2.832 s for the
same `nn_rbm1bJ100` MIR/data-to-bound-executor path.
