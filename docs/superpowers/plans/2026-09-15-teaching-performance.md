# Teaching-model performance investigation

## Current objective and baseline

User authorization: fix the teaching fixtures that are slower than CmdStan,
keeping the combined PR #371. Baseline source is `a77e29ab` (runtime/compiler
identical to main `2ae6c1d0`), immutable binaries in `build-teaching-latest`,
and full run `fae5494c296cd547` under `/tmp/stanli-rethinking/latest`.
There are 38 slower or capped fixtures: one educational, six Rethinking,
and 31 brms. The m14.11 preparation failure is also unresolved (#372).
Recorded performance follow-ups are #373 and #374.

## Evaluator

- Semantics: unchanged model/data bytes and sampler settings; existing
  three-point CmdStan density/gradient/output references; strict paired
  gradient gate 1e-9; no relaxation of recorded exception policies. Internal
  alternative paths should remain bitwise identical unless a justified,
  documented arithmetic-order change is required.
- Performance: complete one-chain CLI execution, including preparation,
  warmup, 1000 draws, generated quantities and full CSV output. Four seeds,
  CmdStan-first per seed, Stanli cap min(3x CmdStan, 900 s), as in the baseline.
  Target is no slower median on the affected fixtures, with complete seeds;
  no survivor averages. Use fixed-point gradients and phase profiles to
  attribute changes. Marginal losses require bounded A/A and counterbalanced
  confirmation rather than repeated attempts until a favorable result.
- Generality: structural rules only; no model-name, source-text, data-value or
  fixture-specific paths. Positive and adversarial near-miss tests, unchanged
  effects/errors, safe fallback and unrelated canaries.
- Delivery: isolated candidate builds, focused commits, applicable CTests and
  full corpus checks, final fresh measurements and updated reports in the PR.
  Historical baselines remain retained. Do not run heavy jobs concurrently
  with timed comparisons.

## First discriminating experiment

Profile the unchanged baseline with STANLI_PROFILE and STANLI_PROFILE_PREP
on ordered logistic, sequential ratio, asymmetric Laplace, probit ordinal,
truncation, Matern GP, Pareto and measurement-error fixtures. Raw outputs live
in `/tmp/stanli-teaching-perf/profiles`. Instrumented timings attribute work;
they are not benchmark results. Stop at a hotspot classification before
choosing the first implementation.

Competing explanations and alternatives:

1. Scalar graph/structured-program overhead versus expensive mathematical
   kernels. Prediction: OP_PROGRAM/structured control dominates the former;
   density/linear-algebra opcodes dominate the latter. Counterproposal to
   faster dispatch: eliminate repeated computation through generic compiler
   specialization or vector kernels, preserving checks and branch effects.
2. Repeated gradient execution versus NUTS trajectory/output/preparation cost.
   Prediction: some CLI losses have near-parity or faster fixed gradients;
   their retained CSV diagnostics and leapfrog counts must be inspected before
   changing kernels. Do not change adaptation/RNG merely to win one seed.
3. Preparation recursion (#372): overlapping liveness pricing subproblems
   versus intrinsically expensive candidate compilation. Compare memoized
   interval decisions to the existing search before changing its policy.
   Memoization must preserve the exact chosen partition, tie handling and
   graph immutability during pricing. A bounded heuristic is an alternative,
   not a semantic-equivalent substitute for caching identical subproblems.

## Profile evidence and first candidate

Baseline profiles attribute 94.6% of m12.5 gradient time to ordered logistic,
91.6% of cumulative-probit time to ordered probit, 99.5% of sequential-ratio
time and 96.4% of asymmetric-Laplace time to structured loops. Normal LCDF
accounts for 67.8% of weighted truncation, GP covariance 76.8% of Matern GP,
and multi-normal 80.7% of m15.7. These are separate mechanisms.

First candidate: record ordered-logistic partials directly through the existing
recorder for a shared cutpoint vector. The unmodified Stan Math template
already computes double partials; both nested tapes and their allocations can
be removed. Counterproposal: cache a unit-seeded nested tape's derivatives,
which removes the second evaluation but retains tape construction. Direct
recording has greater headroom and an existing implementation contract.

Predicate: ordered-logistic opcode with the shared-vector layout. Arrays of
cutpoint vectors retain the established fallback. Keep the same outcome and
location shapes, propto flag, all-active operand types and validation. Reverse
scales each stored partial once, as Stan Math's propagator does. The topology
bit distinguishes disconnected returns from connected zero derivatives.
Proof gate: bitwise nested-tape comparison for scalar/vector locations,
shared/array cutpoints, widths 0/1/3, both propto settings, nonunit and zero
seeds, and invalid ordering followed by successful evaluation. Then compare
the actual target's full gradient with the immutable baseline and CmdStan.

Candidate build: `build-teaching-perf`, matching the baseline Release flags.
The 144-case local oracle and lowering tests pass. Eight fixtures pass all
three recorded CmdStan reference points (558 values, worst relative error
3.25e-13). Five alternating measurements per arm give gradient medians in ms:
m12.5 7.448 -> 2.736 (CmdStan 3.252), m12.6 8.088 -> 2.954 (3.382), m12.7
7.550 -> 2.719 (3.259), hurdle cumulative 0.0184 -> 0.0070 (0.0086).
All tested candidate values and gradients equal the baseline exactly. Two
unaffected canaries differ by about 1% in timing; attribution is unresolved
pending bounded A/A controls. Raw: `/tmp/stanli-teaching-perf/ordered-ab.jsonl`.
Full sampling and full-suite integration remain pending.

## Preparation recursion candidate

`Carver::price_liveness` recursively prices both strict-vocabulary and
least-crossing cuts, revisiting overlapping intervals. Its input graph is
immutable during the search. Cache only each interval's costs and winning
split, then rebuild candidates for the selected leaves once. This preserves
the whole/pressure/strict tie priority and avoids retaining every speculative
compiled program. Counterproposal: cache compiled candidates too; rejected
for this first experiment because large speculative programs would remain
live for the entire search. No heuristic limit or pricing policy changes.

Proof obligations: unchanged selected partition, reported cost, compiled
graphs and gradients; clear the interval cache before graph mutation. Existing
strict-cut, cheap-cut, join-floor and repeated-evaluation tests are the first
oracle. m14.11 must prepare and pass all recorded numerical references; other
teaching fixtures must retain bitwise outputs and unchanged graph structure.
Next experiment: implement compact decision caching, test the carver, then
time preparation with the existing 300-second bound.

Preparation candidate result: m14.11 now prepares in 13.64 seconds (14.00 s
process wall), 78.9 MB maximum RSS. All three recorded CmdStan points pass
(33 values, worst 1.48e-13). Existing carver tests and explicit cached versus
uncached compiled-graph/gradient comparisons pass. Full-suite coverage pending.

## Fixed GP geometry

Matern covariance's backward replay promotes data locations to autodiff,
although their adjoints are discarded. First probe keeps fixed locations
double in the existing Stan Math call, preserving the hyperparameter tape and
weighted output reduction. Predicate: location has no adjoint and family is
Matern 3/2, Matern 5/2 or exponential; active locations and exp-quad retain
their existing paths. Counterproposal: closed-form covariance partials would
remove the remaining tape, but changes reverse accumulation order; defer
until this smaller probe measures remaining headroom. Test exact parity over
all activity masks, scalar/multidimensional and repeated locations, 0/1/5/12
points, then the Matern fixture at the three oracle points and matched timing.

Result: expanded GP tests pass bitwise; Matern fixture's 105 reference values
pass at all three points (worst 3.64e-14). Five alternating gradient medians:
46.62 -> 32.19 us, CmdStan 29.13 us. Candidate and baseline agree bitwise at
the timed point. This reduces the cost but does not yet establish parity.
Raw: `/tmp/stanli-teaching-perf/gp-ab.jsonl`.

## Structured scalar indexing

Sampling profiles show index validation/selection is a major loop cost:
roughly one third of sequential-ratio samples, before accounting for dispatch.
The general index routine reconstructs arbitrary-rank geometry for each scalar
vector access. First probe specializes one fixed-size vector axis with one
scalar selector. Eligibility proves every descriptor/storage invariant that
the general validator checks; invalid descriptors fall back, while the selected
integer and bounds are still checked on every execution. Reads, reverse reads
and in-place writes share the same validator. Multi-index, matrix, range,
dynamic-size and malformed descriptors remain general.

Counterproposal: compile complete loop bodies and branches into register
programs, eliminating version/dispatch overhead too. The existing segmenter
only joins straight-line blocks, and profiles show that broader work has
headroom; first establish the smaller indexing gain and re-profile. Require
direct-kernel parity and exception tests, repeated/copy execution, existing
structured-loop suite and three-point real-model numerical gates.

Result: structured-loop suite and direct general-versus-specialized exception
tests pass. Five alternating gradient medians (us): asymmetric Laplace
13.06 -> 11.31, sequential ratio 301.76 -> 235.41, continuation ratio
299.97 -> 227.88, stopping ratio 300.47 -> 230.28, zero-inflated asymmetric
Laplace 16.50 -> 14.15. All values/gradients equal the pre-index candidate
bitwise and pass the 1e-9 CmdStan gate. This gain is insufficient: these
fixtures remain about 3x slower on gradients. Larger register-program
regions remain an open alternative. Raw: `index-ab.jsonl` in the same folder.

## Piecewise CDF code generation

`sw_trunc`'s sample profile spends over half its samples in libm pow(), versus
roughly 8% for the CmdStan driver. An isolated assembly probe using identical
Clang flags shows the recorder instantiation hoists all sixteen power calls
from mutually exclusive approximation branches; the var instantiation retains
the branch structure. This is excess work, not slower mathematical formulas.
Compiling the probe with `-fmath-errno` restores guarded power calls. Candidate:
apply that flag to the two CDF shards on native Clang only (GCC defaults to it).
Alternative: manually rewrite or approximate the formulas; reject that while
the unchanged Stan Math implementation has this code-generation remedy.
Require bitwise values/partials, all density-signature and CTests, boundary
fixtures, matched gradient benchmarks and full corpus references. Probe
assembly and source are retained as `lcdf-probe*` in the evidence folder.

Five-pair result (us): normal truncation 15.16 -> 5.54 (CmdStan 6.35), weighted
truncation 15.00 -> 6.11 (6.72), interval censoring 4.06 -> 1.50 (2.84).
Candidate and previous values/gradients are bitwise identical. Poisson
truncation and ordered probit remain essentially unchanged, as expected from
their different implementations. Raw: `cdf-ab.jsonl`.

Next density probe: extend direct partial recording to the ordered-logistic
GLM. It has the same propagator semantics and the recorder already supports
matrix edges. Keep all-active types and the existing scalar outcome marker;
test weighted reverse including empty outcomes with a nonfinite output seed.
This targets `sw_cumulative`, whose separate GLM kernel did not benefit from
the first ordered-logistic change. Remove the unused nested GLM branch if the
probe passes; keep binomial and categorical GLMs unchanged.

Ordered GLM result: unit oracle passes including empty data with an infinite
seed; timed medians 25.93 -> 22.06 us (CmdStan 21.58), bitwise internal parity.
The complete corpus replay with these kernel changes passes 316/316 under
the existing policies at three points (1,008,743 compared values). As before,
this count includes documented ill-conditioning, support and domain policies;
it is not 316 strict finite-gradient passes.

Loop representation counterproposal: disabling retained loops speeds the
asymmetric-Laplace fixture from 11.65 to 6.21 us, with relative numerical
difference 1.21e-15, but the ordinal sequential-ratio fallback refuses its
runtime integer index. Thus globally disabling retained loops is not a valid
fix. The probe stopped on that refusal; raw results are retained in
`loop-representation.jsonl`. Investigate bounded data-controlled while
specialization or richer register-program control before changing policy.

Integration checkpoint before the CDF flag: all 249 CTests pass, including all
70 density-signature reference tests. No full sampling gain is claimed yet.

## Sampling-quality interpretation

Retained CSV inspection separates kernel deficits from pathological short
chains. For `sw_re_negbin`, CmdStan seed 2 takes 0.201 s but has 982/1000
divergences and mean 19.8 leapfrogs; other CmdStan seeds take 4.96–5.87 s with
809–972 mean leapfrogs. Stanli's three completed seeds take 3.16–3.62 s;
seed 2 hits its 0.603 s relative cap. `s2_mixture_theta` similarly has two
short CmdStan chains with 642 and 674 divergences, while corresponding Stanli
chains have zero. Keep these outcomes visible and keep the cap; do not change
adaptation or chase the runtime of a pathological chain. `ch14_m14_9` has no
divergences on either side; its small CLI difference accompanies 5.75–7.44
versus 5.06–6.52 mean leapfrogs and near-parity fixed gradients. These require
honest matched-work attribution, not a model-specific kernel rule.

A data-while feasibility probe (not a default) resolves the remaining ordinal
loop control before building the graph. With retained loops disabled, gradient
times are 61.17 us (sequential ratio), 58.56 us (continuation ratio), 58.95 us
(stopping ratio), and 78.80/80.54 us for category-specific thresholds. First
three compare with roughly 74–77 us CmdStan; preparation takes 12–25 ms.
The first sequential-ratio point agrees bitwise with the retained-loop path.
Raw: `data-while-probe.jsonl`. The blanket environment-gated probe was removed:
it lacks a bounded transactional selection policy. Continue with a canonical
counted-while proof or isolated bounded trial; never enable blanket unrolling.

Next density evaluator: use primitive values for ordered-probit forward
(which ignores `propto`/activity types), leaving weighted var replay unchanged.
For multinomial, retain exact Stan Math validation, normalization/summation
and weighted multiply-before-divide reverse order. Compare simplex boundaries,
zero counts, proportional/full normalization, inactive edges, zero/negative/
infinite seeds and invalid simplex with all-var oracle. Require bitwise unit
and fixed-point comparisons before accepting a measured gain.

Multinomial result: five alternating medians 20.29 -> 9.82 us, CmdStan 11.26 us,
bitwise at the fixed point and in the weighted unit oracle. Probit's value-only
forward is neutral (17.87 -> 18.19 us); sampling its CPU stack again identifies
speculated pow calls in std_normal_lcdf. Probe the same math-errno remedy in
its translation unit before retaining the forward change. Raw: `tail-ab.jsonl`;
`profiles/probit-sample.txt`.

### Bounded whole-model specialization experiment

An alternative to a complex local rollback is a fresh lowering over the
already prepared transformed-data environment. Try legacy unrolling plus
pure data-while evaluation in that isolated lowering, with fixed statement,
slot and scalar-storage budgets. On any undecidable while, unsupported
construct or budget exhaustion, discard the entire trial and lower the
original program under the existing policy. Transformed-data RNG, validation
and printing must run once, before either alternative, with independent
mutable shape/interpreter state for the trial. No source/model-name matching.
Initially keep this opt-in while testing. Require a loop with runtime-control
cost to consider a trial; reject non-inlined user calls/effectful bodies in
this first implementation. This targets bookkeeping for bounded scalar
indexed loop bodies, not a blanket policy that all fitting graphs are faster.
Evaluator: baseline-vs-trial repeated values/gradients, empty/single/multiple
trips, aliased writes, changing bounds, early exits, parameter-selected guards,
nontermination/budget exhaustion, transformed-data RNG/print-once, fallback
same graph and fields, existing loop suite, complete reference replay and
five alternating paired target/canary timings including preparation/RSS.

Bounded trial result: five alternating medians (previous -> candidate vs
CmdStan, us): sequential ratio 228.58 -> 61.00 vs 79.26; continuation ratio
230.00 -> 59.78 vs 76.36; stopping ratio 235.51 -> 59.40 vs 71.90; the two
category-specific variants 353.65 -> 81.27 vs 106.65 and 350.96 -> 79.54 vs
109.15. Asymmetric Laplace improves 11.13 -> 5.90 vs 3.42; zero-inflated ALD
14.19 -> 6.32 vs 4.50, still behind. Unrelated measurement-error and truncation
canaries are unchanged within noise. The plain ordinal paths are bitwise
identical; category-specific sequential ratio and ALD change reduction order
by tiny amounts, all below 1e-12 relative to CmdStan. `bounded-ab.jsonl` retains
all pairs. Full 316-policy replay passes at all three points with 1,008,743
values compared. Positive/fallback unit cases preserve TD printing/RNG once,
write_array and layouts; a 20,000-iteration case exhausts the bounded trial
and produces the original graph/fills. Default integration testing follows.

Dirichlet follow-up: its shared-vector signature can use the existing partials
recorder, but compilation exposed two missing interface details: vector
partials need a matrix view and vector_seq_view discovers value extraction by
ADL. Add those generic views, retain array-of-vector fallback and each operand's
activity/propto type, and compare all four activity masks/full and proportional
forms/weighted seeds plus validation recovery against Stan Math. No formulas
are replaced. Full recorder and density-signature suites must pass after the
header change.

Ordered-probit continuation: the first compiler remedy only reduces 17.87 to
15.88 us (CmdStan 5.22). Try retaining std_normal_lcdf's recorded partials and
the log-difference denominators for shared cutpoints. Keep the unmodified
primitive ordered-probit call for validation/value and the existing replay for
arrays of cutpoint vectors. Reverse must apply the actual output seed before
division (log_diff_exp), then multiply by the CDF partial; do not cache a
collapsed unit gradient. Accumulate separate location/cutpoint buffers before
scattering to preserve aliased inputs. Require weighted, boundary, validation
and three-point corpus oracles; reject if this cannot meet the numeric gate.

Dirichlet result: weighted activity-mask/vector-versus-array fallback tests
and recorder tests pass. Five-pair medians 24.17 -> 13.67 us, CmdStan 14.30 us;
bitwise fixed-point equality. Multinomial and ordered-GLM canaries unchanged.
Raw: `dirichlet-ab.jsonl`. The full density suite still needs rebuilding after
the recorder-header addition.

Probit stress oracle: ordinary inputs agree bitwise. At location shifts of
+/-30, recorder versus var CDF instantiations differ by about 6e-14 relative
in a few weighted gradients (largest observed absolute difference 4.6e-12
at a derivative of magnitude 80). Preserve the pinned CDF formulas and use
1e-12 relative for this internal gradient oracle, stricter than the unchanged
1e-9 external gate. Values remain exact. The same far-tail tolerance also applies to the unchanged
array-cutpoint replay: the library and oracle translation units differ in
the Clang math-errno setting, so the var approximation rounds differently too.

The first probit-partial variant measures 15.17 -> 9.63 us (CmdStan 5.24),
bitwise at the corpus point, but recomputes each CDF once for value and again
for partials. Next remove that duplication: use the same shared-vector checks,
then sum the recorded CDF values/log differences in the density's order.
Keep the generic Stan Math replay for every other layout. Add aliased-edge
and far-tail oracles; do not relax the external gate or approximate CDFs.

Final shared-vector probit result: five-pair median 15.87 -> 4.55 us versus
CmdStan 5.24 us; fixed-point LP/gradient remain bitwise identical. Weighted
shared/array cutpoints, +/-30 tail inputs, aliased inputs and validation/error
recovery tests pass. `probit-native-ab.jsonl` retains all samples. The native
reverse uses separate operand buffers before scatter, retaining caller alias
semantics; no density approximation or sampler setting changed.

Integration narrowed the specialization selector: the cross-path oracle found
one ULP of island/no-island drift when a range slice became statically exposed.
Range/gather selectors now decline the whole trial; the original graph and
fills are asserted unchanged. The retained-plan suite explicitly disables the
whole-program alternative so it continues to exercise retained plans. All
seven measured ordinal/ALD cases still specialize. No cross-path tolerance or
reference was relaxed. Fresh R ecosystem acceptance passes with the built CLI
explicitly configured; the first attempt correctly failed for a skipped CSV
oracle when that path was absent.

Wiener follow-up: the retained graph has forty scalar Wiener calls. Its
existing kernel builds a tape for forward, discards it, then builds another
for reverse, promoting even parameter-independent outcome/bias to var. Probe
an all-scalar signature with those two adjoint edges absent: keep boundary,
nondecision time and drift active, call the same Stan Math template once and
retain its unit partials. Other signatures/active outcomes keep replay. This
is selected by shape/adjoint edges, not the recorded model or values. Weighted
unit comparisons (1e-12 relative), near-boundary observations, active-edge
fallback and validation failures must pass, followed by paired timings and
the unmodified three-point external gate.

The first Wiener probe did not apply to the target (45 us unchanged): bias
is active in its graph. Correct the hypothesis to a fixed outcome with all
four distribution parameters active, retaining four partials. Exercise all
four outcome/bias activity combinations, including replay for active outcomes.

Wiener result: five alternating pairs give 46.07 -> 28.34 us versus
CmdStan 29.37 us, with bitwise fixed-point value/gradient equality. The probit
canary remains faster than CmdStan. All four outcome/bias activity masks,
full/propto, zero/negative/infinite seeds, near-boundary observations and
invalid outcomes pass their oracle. Nonfinite seeds or cached partials retain
the original weighted replay. Raw samples: `wiener-four-ab.jsonl`.

Final Wiener review found a missing refusal case: finite seeds of 1e308 can
produce a finite post-scaled unit partial where the original weighted reverse
produces Inf or NaN through intermediate overflow. The expanded oracle
reproduces this. Limit cached reverse reuse to a unit output seed; every other
seed follows the original weighted replay, as do nonfinite cached partials.
This preserves the corpus target's fast path without approximating weighted
semantics. Add 1e308 and 1e-308 to the weighted oracle. The partial sweep
`6f0119ad091842b3` was interrupted before completion and is retained separately;
none of its timings will enter the final report. A fresh immutable run follows
this correction and the integration checks.

The extreme-weight tests now pass. Five new alternating pairs retain the
Wiener gain: 45.02 -> 29.23 us, CmdStan 28.99 us, bitwise fixed-point equality.
That is near parity (0.8% slower in this repeat), rather than a stable claim of
being faster. `wiener-unit-ab.jsonl` contains all pairs; weighted overflow and
underflow follow the unchanged reference arithmetic.

Full-platform CI on ea4d7e24 exposed two A/B harness integration issues. Its
saved 316-model artifact has 88 failed profile samples solely because completed
specializations emit `bounded_log_prob`, while the consumer required `log_prob`.
The consumer now selects the completed graph without rewriting raw trace rows;
a refused trial cannot supply stages missing from the ordinary fallback. All
30 parser/harness tests pass, and replaying the saved profiles eliminates those
88 failures. The remaining failures are experimental source-pass preparation
of m14.11 exceeding the workflow's 30-second limit; the shipped source-pass-off
path prepares in roughly 25 seconds on that GCC runner and passes reference
checks. Restore the harness's standard 300-second preparation limit (the same
as reference replay), retain every model and numerical/gradient gate, and add
m14.11 plus a successful specialized graph to the PR slice. This does not alter
the separate three-times-CmdStan sampling cap. The next CI run must establish
whether experimental preparation finishes and still matches its oracle.

These harness/workflow-only changes do not change the frozen ea4d7e24 runtime,
compiler, benchmark sources or inputs used by full timing run a224d12afe02ac98.
