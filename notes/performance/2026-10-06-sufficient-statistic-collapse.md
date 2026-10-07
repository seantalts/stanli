# Sufficient-statistic collapse for fast mode: design

Status: design for review, 2026-10-06. Nothing here is implemented. Base:
`origin/fastmath/mode` at `73a344cd`. The sizing behind it is a census of the
shared corpus on that revision; outputs and scripts are in
[data/2026-10-06-suffstat-census/](data/2026-10-06-suffstat-census/). Every
"ceiling" below is an estimate from that census; group counts, N, P,
conditioning, high-precision errors and the two hand-collapse timings are
measured. Context: [numerics versus speed](2026-10-05-numerics-vs-speed.md).

## Recommendation

Add one fast-mode preparation pass that, with the data in hand, replaces a
likelihood term over N observations by an equivalent term over G groups, and
three ways to evaluate the grouped term:

1. **Weighted evaluation** (any density): evaluate the existing kernel on one
   representative per distinct row and take a weighted sum.
2. **Grouped normal** (and lognormal): per-group count, mean, centred sum of
   squares and rounding residual.
3. **Linear-Gaussian**: a normal term whose mean is affine in the parameters
   becomes a quadratic form centred at the least-squares solution.

Default mode is untouched. Dedicated count kernels for Bernoulli, binomial and
Poisson, and every other exponential family, are left out on the evidence
below.

## Evidence

342 application models analysed, 341 with timings. Models at or above each
estimated whole-gradient ceiling against the shipped default, each family
taken alone:

| family | >=2x | >=10x |
| --- | ---: | ---: |
| weighted evaluation of duplicate rows | 22 | 9 |
| grouped normal | 29 | 18 |
| linear-Gaussian (includes grouped normal with one sigma) | 58 | 25 |
| linear-Gaussian, net of grouped normal | 34 | 5 |
| Bernoulli/binomial counts | 10 | 4 |
| Poisson sums | 3 | 1 |
| lognormal | 1 | 0 |
| all of the above together | 85 | 36 |

The census's two orderings with net and cumulative counts are in
`summary-linear-gaussian-and-families.txt`.

- After linear-Gaussian and weighted evaluation, Bernoulli/binomial count
  kernels add 2 models at >=2x and Poisson sums add 2. Not built; recorded
  under "Left out".
- Linear-Gaussian is the largest family alone (58 at >=2x, 25 at >=10x) and
  subsumes grouped normal with one shared sigma. 23 of its 58 are N < 200
  models whose gain is fusing the term into one op, and 13 need the sparse
  structure described below.
- The estimates were conservative on the three models collapsed by hand and
  timed with `bench_grad` (shared machine, so rough): radon_county 29.7x
  measured against 15.6x estimated, ch12_m12_5 234x against 80x (part of that
  is a missing vectorisation of the `ordered_logistic` loop), diamonds 65.8x
  against 18.9x.
- Existing passes do not overlap: re-rolling, gather run compression and the
  fused kernels lower the per-element cost and never reduce N. The one overlap
  is fast-mode CSE merging identical scalar ops; net of it the corpus count at
  >=2x falls from 56 to 49 (first census, before linear-Gaussian).

Limits of the census: it ran with islands, partitioning, region maps,
structured loops and CSE off so that scalar structure was visible, so it shows
what is collapsible, not what the pass will see at its place in the shipped
pipeline. The compiler half of fast mode (stanc3 partial evaluation) was not
in the measured MIR. 38 models keep their likelihood in user-defined functions
or hand-written target terms the census could not see into.

## Design

### Where it runs

A new pass, `collapse_observations`, in `Lowering::optimize` after the
post-re-roll in-place pass and before `partition_lanes`
(`runtime/src/lower.cpp`), under `compile_options.fast_math` only. At that
point the collapsible terms are plain ops: for example radon_county is
`GATHER -> NORMAL_LPDF`, earn_height is `INDEX, INDEX, MUL, ADD ->
NORMAL_LPDF`, and ch11_m11_4 is 504 scalar `INDEX, INDEX, ADD, INV_LOGIT,
SET_INDEX_INPLACE` chains feeding one `BINOMIAL_LPMF`. Partition, CSE and
islands then work on the smaller residue. Only the log-density graph is
rewritten; `write_array` (generated quantities, `log_lik`) is a separate graph
and is not touched.

Disable with `STANLI_NO_COLLAPSE=1`. The pass reports to the preparation trace
like its neighbours, and per term to the execution report: N, G, the evaluator
chosen, or the refusal reason.

### Step A: element value numbering

For each candidate term (a density op with a data variate of length N >= 16
whose output is summed into the target), number every element of its argument
cone so that two elements get the same number only if they are the same
expression over the same parameter elements and bitwise-equal data values.

- Exact, not hashed: a table keyed on (opcode, variant, operand value numbers
  or data bit patterns), as `cse.cpp` does per op. A hash collision must not be
  able to merge two different observations.
- Slots are mutable buffers. As in CSE, each write bumps a version, tracked
  here per element so that `SET_INDEX_INPLACE` chains resolve to the scalar
  that was stored.
- Transparent ops: elementwise unary and binary ops, `GATHER`, `INDEX`,
  broadcast, the store/load chains, and a matvec row (keyed on the row's data
  bit patterns and the parameter vector). Any other producer gives each of its
  elements a fresh number, so an unknown op can only prevent grouping.
- NaN in the variate or in a data operand declines the term, leaving the
  original op to report it.

Rows are grouped by the tuple of argument value numbers: including the
variate for weighted evaluation, excluding it for the normal forms. Groups are
ordered by first occurrence, so the result is deterministic.

The same walk records, for normal terms, each mean element as an affine form
over leaves (a parameter element, or any active value that is not itself
affine) with data coefficients. That is the input to the linear-Gaussian step.

### Step B: restriction

If G <= N/2, build the term over representatives: each transparent op in the
cone is re-emitted at length G with its data operands restricted to the
representative rows (new fills) and its gather indices restricted likewise;
scalar chains keep the representatives' ops and a new G-length pack. The
original cone is left in place and removed as dead if nothing else reads it.
If something else does read it (a second likelihood on the same predictor),
both stay, and the term still collapses.

Refuse, and keep the original term, when: the density's output feeds anything
but the target sum (mixtures, HMM, marginalisation), the term is inside a
`reduce_sum` child graph or a retained loop, the cone contains an effectful
op, or a length is dynamic.

### Step C: evaluators

**Weighted evaluation.** The original density opcode at length G in its
elementwise-lp form, then a dot product with the data weights (group sizes).
No new density kernels. To verify before building: that each density's
elementwise form runs the reverse pass with per-element seeds; where one does
not, the term is refused.

**Grouped normal.** A new kernel taking, per group, n, the mean as a
double-double (ybar and the rounding residual d = sum(y - ybar)), and
S = sum((y - ybar)^2), all computed once in double-double at preparation. With
r = ybar - mu:

- lp = -n log(sigma) - (S + 2 r d + n r^2) / (2 sigma^2), plus the constant
  when not `propto`
- d/dmu = (n r + d) / sigma^2
- d/dsigma = -n / sigma + (S + 2 r d + n r^2) / sigma^3

mu and sigma may each be per group or a broadcast scalar. Lognormal is the
same kernel on log y, with the Jacobian term a preparation-time constant.
Raw moments (sum y, sum y^2) must not be used: they reach 1e7 to 1e16 ULP.

**Linear-Gaussian.** For a normal term with one broadcast sigma and mean
`c + Z theta`, applied to the grouped rows when grouping already reduced N
(rows weighted by sqrt(n_g)). Preparation computes the least-squares solution
theta_hat, d = Z'(y - c - Z theta_hat), RSS, and R = qr(Z). Per gradient, with
w = R (theta - theta_hat):

- Q = RSS - 2 (theta - theta_hat)' d + w'w, then lp = -N log(sigma) -
  Q / (2 sigma^2)
- dQ/dtheta = 2 (R'w - d)

Two shapes, chosen from the design at preparation:

- dense, when P^2 < nnz(Z): O(P^2) per gradient;
- arrow, when Z is an indicator block of G columns plus K dense columns (the
  hierarchical radon models, i319_gauss_re, rats): O(K^2 + G K).

Refuse when neither is cheaper than the rows (23 of the 112 qualifying models,
for example prophet with P = 1203 against N = 1169). Cap P for the dense shape
so preparation stays bounded; the cap is set from the preparation measurements
in step 3 of the plan.

### Numerics

Errors against a 70-digit mpmath reference, in ULP of the largest gradient
entry.

Grouped normal, worst of three corpus points:

| model | current path | centred with residual |
| --- | ---: | ---: |
| radon_pooled (5 groups) | 102 | 5.0 |
| radon_county (386 groups) | 33.5 | 2.7 |
| earn_height (20 groups) | 7.5 | 0.5 |
| ch09_m9_4 (1 group) | 8.3 | 4.3 |

Without the residual, a synthetic stress with data shifted by 1e3, 1e6 and 1e9
degrades to 367, 4e5 and 1e9 ULP; with it, 3 to 5.

Linear-Gaussian, near the mode (least-squares theta times 1.001, sigma at the
residual sd), where the forms separate; away from the mode all of them are
within 14 ULP:

| model | cond(Z) | per-observation | raw Gram | QR of [Z y] | least-squares centred |
| --- | ---: | ---: | ---: | ---: | ---: |
| kilpisjarvi (N=62, P=2) | 8.9e5 | 126 | 1.7e3 | 1994 | 5.0 |
| kidscore_interaction (N=434, P=4) | 3.1e3 | 44 | 1.0e3 | 943 | 28 |
| diamonds (N=5000, P=25) | 715 | 98 | 1.6e4 | 3685 | 3.7 |
| mesquite (N=46, P=7) | 27 | 36 | 36 | 26 | 26 |

(Preparation in double precision. The per-observation column is a float64
loop standing in for the current path, which `stanli_check` could not be
pointed at near the mode.) Raw Gram and the augmented QR both cancel near the
mode and are rejected. The centred form is at least as accurate as the
per-observation path there.

Not yet checked: the centred form on a rank-deficient design. 44 of the 112
qualifying designs are rank-deficient (non-centred or hierarchical columns).
Evaluating the quadratic form does not need full rank, but theta_hat is then
not unique; step 3 starts by settling this (minimum-norm solution from a
rank-revealing QR is the expected answer) against the reference.

Weighted evaluation changes only the order of the target sum.

### Errors and rejection

Fast mode keeps whether a point rejects and how a non-finite result is
classified. A collapsed term validates the same arguments on G rows, and maps
the failing representative back to its first original observation so the
message names an index that exists in the user's data. Which of several bad
observations is reported first may differ from default mode; this is the same
concession the numerics note records for data-class specialization.

Data-only checks on the variate run on all N values at preparation.

### Preparation cost and memory

The walk is linear in the cone's elements, plus one QR of G x P for the
linear-Gaussian form. It runs only in fast mode and only for terms with
N >= 16. Collapsed terms drop the N-length value and adjoint buffers of the
cone. Both are measured per step (below), on small models as well as the large
ones, since AGENTS.md does not allow a large-model win to tax ordinary models.

## Plan

Each step is its own change with the full corpus benchmark the compiler
pipeline rule requires, run in fast mode against fast mode without the step.

0. **Dry run.** Steps A and the refusal checks in the real pipeline position,
   reporting what would collapse and why not, with no rewrite. Compare against
   the census's 118 models. This is where the visibility question is answered:
   how many terms are already inside retained loops, region maps or islands
   when the pass runs, and whether the pass must move or those producers must
   yield.
1. **Restriction and weighted evaluation.**
2. **Grouped normal and lognormal.**
3. **Linear-Gaussian, dense**, starting with the rank-deficient check and the
   preparation-time cap.
4. **Linear-Gaussian, arrow.**

### Tests

- Pass: fixtures for each transparent op, each refusal, shared cones, scalar
  chains, bitwise data equality (-0 against +0), NaN data, first-occurrence
  order.
- Kernels: against a high-precision reference over shifted, scaled and
  near-mode inputs, including the stress cases above, with a bound in ULP of
  the largest gradient entry and a tighter one on the log density.
- Models: the fast-mode gate in TESTING.md on every corpus model, and fast mode
  with and without the pass compared at the same points; identical rejection
  and non-finite classification.
- Sampling: posterior agreement with default mode on the corpus models that
  have reference posteriors.

Dependency: the fast-mode gate is defined in
[TESTING.md](../../TESTING.md#fast-mode) (1e-12 on the log density and on the
gradient scaled by its largest entry, against the recorded CmdStan values) but
is not yet enforced. This work can start on kernel-level references and the
fast-against-fast comparison, and should not ship to users before the gate
runs in CI.

## Left out

| family | corpus evidence |
| --- | --- |
| Bernoulli/binomial counts, Poisson sums | 2 models each at >=2x beyond weighted evaluation |
| gamma, beta, von Mises, Dirichlet, multinomial | present in 1 to 3 models, none collapses (per-observation predictors or distinct parameters) |
| categorical | 5 models, groups = N in all |
| multi_normal scatter | 8 models, none has a shared mean over more than one row |
| exponential, Weibull | 1 model each; under an HMM, or active shape |
| neg_binomial_2 | phi never data; covered by weighted evaluation only |
| inverse-gamma, Cauchy, chi-square, Rayleigh, Pareto | absent as data-variate terms |
| likelihoods inside user-defined functions | 38 models invisible to the census; 11 have >=2x repeated raw data rows |
| normal with per-group sigma and an affine mean | not sized |

Add any of these when a model that needs it appears, with its measurement.

## Open questions for review

1. Order: weighted evaluation first (machinery with no new math) as proposed,
   or grouped normal first (more models)?
2. Is the first-reported-observation concession acceptable in fast mode?
3. Threshold: N >= 16 and G <= N/2 are starting values, to be set from the
   step-1 benchmark.
