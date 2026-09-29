# Where the passes were too narrow for vectorized MIR

The 0.12.0 compiler pin brought stanc3's wider `vectorize_loops` pass, and
three corpus models got slower. Each fix on `feat/vectorize-consumption` is a
point fix for one shape. This note records the general assumption behind
each one, the class of models the corpus does not show, and what the general
version looks like. The corpus is 254 models and is not representative;
the classes below are what to design for.

## 1. Indexed assignment lowers through a case ladder

`lower_stmt.cpp` lowers an lvalue with indexes by matching a short list of
spellings: `m[i] = row_vector`, `m[:, j] = v`, `a[i, :] = v` on an array of
vectors, `v[lo:hi] = w` on a one-dimensional value, `v[idx] = w` with an int
array. Everything else goes element by element. The fix added the matrix
spelling `m[i, :] = v` by dropping the trailing `All` and reusing the row
case.

Class hidden by the corpus: any lvalue whose static selector is a contiguous
or strided window or a gather, in any dimension: `m[lo:hi, j]`,
`m[i, lo:hi]`, `m[lo:hi, :]`, `a[i, j, :]` on a three-dimensional array,
`m[idx, j]`. Every one of these is a single store today on the read side,
where `try_static_selector` and `builtin_index_map` already classify a
selector as contiguous, strided or gathered.

General version: lower every indexed lvalue through the selector machinery
the reads use. A static selector becomes one `SET_SLICE`,
`SET_SLICE_STRIDED` or scatter store; the ladder goes away. About a day,
and it deletes more than it adds.

## 2. Statically dead statements reach lowering

A statically empty range assignment `x[1:0] = rhs` lowered its whole right
hand side. The pass makes that statement from `for (i in 1:0)`, which
upstream's dead code elimination does not remove. The fix checks the range
before lowering the right hand side.

Class: any statement whose static bounds make it a no-op: zero-trip loops
inside inlined functions with a constant size argument, empty slices in
transformed data, conditionals on constant data flags.

General version: two halves. Upstream, `vectorize_loops` or dead code
elimination should drop a loop whose bounds are statically empty; that is a
few lines in stanc3 and helps every backend. In stanli, lowering should
treat a statically empty selector as a no-op wherever it appears, not only
on an assignment's left hand side.

## 3. The island machine speaks scalars

`island.cpp` admitted arithmetic only with scalar operands, and vector
unaries only for `log` and `exp`, which have range instructions. A single
short vector `+` in the middle of a scalar run ended the run, and the pieces
priced worse than the whole. The fix compiles elementwise `+ - * /` up to 64
elements wide as one instruction per element, and re-carves a refused run
at scalar granularity so a wide vector op cannot make things worse than
before.

Class: any model that mixes small vector expressions into long scalar
regions: state space and hidden Markov transformed parameters, per-row
softmax and log, mixtures over a handful of components, anything the
upstream pass rewrites inside a scalar loop body. Vector `inv_logit`,
`sqrt`, `square`, `fma`, and every binary wider than 64 still end a run.

General version: range forms for every elementwise op the machine speaks,
with broadcast and no cap, so a run never ends at an elementwise op. The
cost estimate also needs recalibrating before it can choose between a joined
run and its split: it charges absorbed constants as registers and prices
`DOT`, `SOFTMAX` and a density call like a scalar add, which is why it
ranked iohmm_reg's split above its join. With a trustworthy estimate the
refusal fallback becomes a real choice between the two carvings.

## 4. Re-rolling only sees lanes one element wide

`reroll.cpp` packs a repeated template whose lanes differ by fresh output
slots, per-lane scalar constants and advancing `INDEX` immediates. A lane
input that is a row of a matrix (`SLICE_STRIDED` with the row advancing)
never matches, and a constant lane wider than one element is rejected. That
is why dogs stays at thirty per-dog densities after the pass vectorizes its
inner loop: the outer loop's lanes are vectors, and neither reroll nor
partition packs them. Partition also declines on a cost model that assumes
packing needs a gather.

Class: every per-row loop, which is one of the most common shapes in
applied Stan: `for (i in 1:N) y[i] ~ multi_normal(mu[i], Sigma)`,
`for (n in 1:N) target += categorical_logit_lpmf(y[n] | X[n] * beta)`, a
likelihood per subject over an array of vectors, per-group Gaussian
processes, and everything the upstream pass now emits as a row statement
inside a remaining outer loop. Today each of these runs one vector op per
row.

General version: vector lanes. A lane may be `C` elements wide. A
lane-varying input that is row `l` of an invariant base collapses to the
base itself, the strided analog of today's `index_elision`; constant lanes
of width `C` concatenate in the base's storage order so no gather is needed;
a term density over the lanes becomes one density over `L * C` elements
reading the container in place. Partition's cost model should then price
a slice-covered base as free. This is the largest item here and the one
that covers the most models: a few hundred lines in reroll and partition.

## 5. The test paths do not run the production compiler

`stanli_check` and the lit runner compile with the external pinned `stanc`
at `--O1`, which does not run `vectorize_loops`. Only the vectorize harness
probe and the shipped entry points run the pass. A lit case therefore
cannot be written as the loop a user writes; it has to spell the statement
the pass produces. The corpus conformance checks likewise verify the
pass-off MIR.

General version: one compile path. `stanli_check` should compile through
the embedded pipeline by default, the lit runner with it, and the external
binary should be an explicit opt-in for A/B work. That deletes a second
way to build a model from the test tree and makes every corpus check
exercise what ships.

## 6. What upstream leaves to stanli

Forty corpus models are untouched by the pass and still re-rolled. The
blocking shapes, in order of how cheap they are to fix in stanc3:

- a statement reading what an earlier statement in the same iteration
  wrote, `mu[n] = ...; target += normal_lpdf(y[n] | mu[n], s)`, every radon
  variant;
- a scalar or int temporary in the body, `real mu = ...`, `int r = rat[n]`;
- two loop-dependent indexes, `x[a[n], b[n]]`, which has no Stan syntax
  for the gathered form;
- a density inside an expression, `log_mix(theta, normal_lpdf(...), ...)`,
  which Stan Math cannot express elementwise, so this class is stanli's for
  good and is what reroll's element density disposition exists for;
- arrays in arithmetic, `alpha - beta * pow(lambda, x[i])` on an array.

The first two are worth asking for upstream. The fourth is why reroll's
vector lanes matter more than any single upstream change: it is the only
place a mixture over a scalar loop becomes one vector op.

## Order

Vector lanes (4) first: largest class, and it also makes the dogs number
go away. Then the lvalue selector (1), which deletes code. Then the island
range forms and estimate (3). The test path (5) and dead statements (2) are
small and can ride along.
