# Sufficient-statistic collapse for fast mode: design

Status: implemented 2026-10-07 except the arrow-structured form (step 4);
see "Result" at the end for what was built, what changed from this design,
and the measurements. Design approved 2026-10-06. Base:
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

Dependency: the fast-mode gate in [TESTING.md](../../TESTING.md#fast-mode)
(1e-12 on the log density and on the gradient scaled by its largest entry,
against the recorded CmdStan values) runs with `tools/verify_refs.py
--fast-math` but is not in CI. The collapse should not ship to users before
it is.

## Step 0 result: what the pass sees (2026-10-06)

`analyze_collapse` (`runtime/src/collapse.cpp`) is the analysis half of the
pass. It runs where the rewrite will, changes nothing, and reports under
`STANLI_COLLAPSE_REPORT=1` in fast mode. `harnesses/collapse_census.py` runs
it over the corpus; its output is `dry-run.txt` in the data folder.

- 46 of 352 models have a term that would collapse, 28 of them by 10x or more
  in that term.
- Of the 56 models the census put at an estimated 2x or more, the pass finds
  39. Seven others it finds were below 2x in the census.
- Visibility is not what the other 17 lack: none is hidden by a region map,
  retained loop or island at this point in the pipeline. They need coverage
  the first version of the analysis does not have:

  | models | what is missing |
  | ---: | --- |
  | 5 | the density feeds a mixture or marginalisation (`LSE2`), so the repeat is in the whole target term, not the density: Mb_model, Mt_model, M0_model, ch12_m12_3, ch12_m12_3_alt |
  | 4 | GLM densities, whose rows are a design-matrix row and an outcome: nes_logit_model, i319_pois_fixed, i319_pois_re, i319_gauss_re |
  | 4 | scalar `ordered_logistic` terms sharing one cutpoint vector: ch12_m12_4 to ch12_m12_7 |
  | 2 | no density op at all, hand-written target terms: sw_acat, ch11_m11_7 |
  | 2 | a predictor built by an op the analysis does not follow: s2_mo_simo_prior, sw_mono |

  The first and fourth rows are one extension: number the target terms
  themselves and weight the ones that repeat. Fast-mode CSE already merges
  the repeats that are the same slots; this would add the ones equal only by
  data value.
- The pass follows elementwise ops, gathers, element and slice stores and
  reads, and data-matrix times vector by row. Slice stores mattered: without
  them it found 43 models, because `vector[N] mu = ...` lowers to a slice
  store that a later pass removes.

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

## Decided in review (2026-10-06)

1. Build weighted evaluation first, then grouped normal, then
   linear-Gaussian.
2. A collapsed term may report a different observation first when several are
   invalid.
3. The fast-mode gate is 1e-12 on the log density and on the gradient scaled
   by its largest entry ([TESTING.md](../../TESTING.md#fast-mode)).

Still to be set from the step-1 benchmark: the N >= 16 and G <= N/2
thresholds.

## Result (2026-10-07)

Built: the pass (`runtime/src/collapse.cpp`, `collapse_linear.cpp`), the two
closed-form kernels (`runtime/kernels/collapse_kernels.cpp`), tests
(`tests/test_collapse.cpp`), and the description in
`runtime/src/OPTIMIZATIONS.md`. It runs in fast mode only.

### Measurements

i9-13900K, one P-core, clang 18.1.3, Release, `fastmath/mode` plus this
work. Fast mode with the pass on against fast mode with
`STANLI_NO_COLLAPSE=1`, the same binary, `harnesses/ab_bench_corpus.py
--fast-math --arm-env off:STANLI_NO_COLLAPSE=1`, 9 rounds, with a third arm
of an identical binary as control (at most 1.4% apart).

- 92 of 352 corpus models have a term that collapses: by rows in 30, by
  groups in 18, by the linear form in 44.
- Of the 91 that could be timed (`dogs_log` has a non-finite benchmark
  point in every configuration), gradients are 8.1x faster in geometric
  mean; 84 are at least 1.1x faster, 67 at least 2x, 41 at least 10x.
- The census estimated 85 models at 2x or more and 36 at 10x or more for
  these families together. The estimates were conservative per model, as the
  hand-collapsed timings suggested, and the pass reaches fewer of the
  scalar-term models than the census assumed.
- The models the pass does not change have the same op graph with it on or
  off, so they were not re-timed in the final run. An earlier full-corpus
  run was too noisy to use (identical binaries differed by up to 41% on a
  loaded machine).
- Fast-mode gate (`tools/verify_refs.py --fast-math`): 351 of 352, the same
  single failure (`sw_gp`) as without the pass and as default mode on this
  toolchain. Largest deviation in the corpus: 1.7e-13 (`election88_full`).
- Object code: about 205 KB of text (110 KB the pass, 85 KB the
  least-squares preparation, 10 KB the kernels).

### What changed from the design

- **Scalar target terms** were added (decided in review after step 0): terms
  with equal values are computed once and multiplied by their count. This is
  what reaches the unrolled `ordered_logistic`, capture-recapture and
  hurdle models. Two things were learned the hard way:
  - Listing one slot twice among the target terms instead of multiplying
    gives wrong results; the later passes take target slots to be distinct.
  - Merging only some repeats is worse than merging none: partitioning then
    fuses the remaining lanes where it had left them to CSE
    (`dogs_hierarchical` 0.68x, `multi_occupancy` 0.79x). Merging is
    all-or-nothing per model on an estimate of dispatches saved, which also
    declines when the repeats read the very same slots and CSE would merge
    them for nothing.
- **Arguments are restricted by pushing a gather upward**, not by re-emitting
  a cone from value numbers. Value numbering only decides which rows are
  equal; each restriction rule is a local identity with a plain gather as the
  fallback. A rewrite that could only use the fallback is taken back unless
  the rows are at least 4x fewer.
- **The linear form applies to grouped rows and to `normal_id_glm`.** Its
  centre is the minimum-norm least-squares solution, and the expansion is
  exact for any centre, so rank-deficient designs (44 of the 112 qualifying
  in the census) needed nothing special. Checked against extended precision
  near the mode with a repeated column: within 1e-13 of the largest gradient
  entry.
- **Thresholds.** Rows 2x fewer than observations for densities with a native
  elementwise form, 8x for those that pay a recorder call per row; groups 2x
  fewer; at least 16 observations. These came from the measurements above,
  not a separate sweep.
- **Dead-op removal follows index reads element by element.** A loop of
  `mu[n] += ...` reads each element back, which kept every store alive under
  slot-level liveness (`sw_mono` was 3% slower until this).

### Not built

| what | why |
| --- | --- |
| Arrow-structured linear form (an indicator block plus dense columns) | The hierarchical radon models it targets already get 7x to 16x from the group form. Not sized beyond the census. |
| GLM densities other than `normal_id_glm` by rows (`bernoulli_logit_glm`, `poisson_log_glm`) | Needs the GLM rewritten as a matrix product and an elementwise density, whose argument checks differ from the GLM's for infinite parameters. `nes_logit_model` (1179 observations, 10 rows) is the main case. |
| A predictor built by ops the analysis does not follow (`ch12_m12_6`, `s2_mo_simo_prior`, `sw_mono`'s monotonic effect) | Each needs its op modelled. |
| Dropping data that no op reads any more | The full-length data stay in the bound buffers; only op-written slots are released. |
| Preparation-time cap for large designs | The linear form is limited to 256 parameters and the analysis to 4 million affine terms; no corpus model comes near either. |

