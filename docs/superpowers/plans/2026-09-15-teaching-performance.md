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

PR CI on f2d1fd38 now passes, including the expanded 46-model A/B slice with
zero semantic or measurement failures. On its Clang Linux runner, m14.11
prepares in 17.0–17.3 seconds with the source pass off and 21.6–23.4 seconds
with it on. The saved artifact is `pr-ci-vectorize-fixed/` in the research
evidence folder. Full release-GCC/platform validation is still running.

The full release-GCC A/B run also passes on f2d1fd38: 316 models, 948 points,
zero semantic, infrastructure, or gradient-performance failures. m14.11 takes
22.1–23.7 seconds with the source pass off and 34.0–35.1 seconds with it on.
This confirms the earlier failure was the 30-second preparation budget; the
numerical tolerance and every model were preserved. All five platform builds
passed, allowing the downstream R acceptance/startup jobs to run. Full saved
measurement artifact: `full-ci-vectorize-fixed/`; run 35047000140.

Cross-platform R acceptance passes on macOS and Linux. Windows reaches the
independent CSV oracle but its CLI cannot locate stanli-compile.exe: the
workflow unpacked the CLI into a different directory from the runtime bundle.
All preceding native/live-method and ecosystem tests passed. Install the
independently built CLI beside the runtime bundle's compiler and supporting
DLLs, preserving the oracle and all no-skip assertions; rerun platform CI.
Fresh startup measurements already recorded: macOS CI 0.262 seconds median
(0.236, 0.273, 0.262); Linux CI 0.248 (0.248, 0.248, 0.244), R 4.6.1.

The same full run passes all 249 CTests under AddressSanitizer and all three
threaded tests under ThreadSanitizer. Its only failed job is the Windows R
oracle deployment described above. Corrected platform run 35049845476 at
f5b047de has started; the runtime and R implementation are unchanged.

### Cross-platform validation, 16 September

Full manual workflow [35049845476](https://github.com/seantalts/stanli/actions/runs/35049845476)
is green at `f5b047de`; runtime/compiler sources remain those measured at
`ea4d7e24`. This includes all platform builds, the complete compiler comparison,
AddressSanitizer, ThreadSanitizer, and runtime R acceptance on Linux, macOS and
Windows. The Windows CLI oracle now runs from the unpacked runtime directory,
beside its compiler and supporting DLLs.

Fresh R-session medians from that same workflow, each using three processes
and four chains with 1000 warmup plus 1000 retained draws, are 0.192 s on macOS
ARM64, 0.236 s on Linux x86_64, and 0.280 s on Windows x86_64. All use R 4.6.1.
Raw CSVs are retained under `/tmp/stanli-teaching-perf/ci-f5-{mac,linux,windows}`;
installation and plotting are outside the measured interval.

The local full sampling sweep is still running. No rebuild or heavy local
profiling runs alongside it. After its postprocessing and final checks finish,
the queued residual investigation will profile the remaining preparation and
kernel costs. The evaluator and competing hypotheses are retained in
`/tmp/stanli-teaching-perf/remaining-plan.md`; its isolated multivariate-normal
recorder probe is not a production change or a validated capability yet.

### Complete performance checkpoint: run a224d12afe02ac98

All 199 fixtures finished the protocol on the frozen `ea4d7e24` build. There
are 192 complete engine comparisons, 178 with lower Stanli median CLI time,
14 slower completed cases, two sampling caps, and five failures before
sampling. The original sweep had 183 complete comparisons and 155 Stanli wins.
No partial-seed aggregates or changed numerical gates were used.

| Collection | Complete | Lower Stanli CLI | Median C/S | Screen clear in both |
| --- | ---: | ---: | ---: | ---: |
| Educational | 13/13 | 12/13 | 1.30 | 10/13 |
| Rethinking | 61/62 | 58/61 | 1.52 | 50/61 |
| brms | 118/124 | 108/118 | 1.45 | 76/118 |

The capped fixtures are m13.6 seed 3 (2.5747-second limit) and sw_re_negbin
seed 2 (0.6507-second limit). The strict pre-sampling failures retain the three
ill-conditioned GP cases, unsupported COM-Poisson and the inverse-Gaussian
domain refusal. Full diagnostics and per-seed records are published alongside
the tables; the raw 502,030,085-byte archive is retained locally with checksum.
The four-page McElreath PDF was regenerated and every rendered page inspected.
Post-run identities, educational checks, guide examples and R adapter/startup
measurements all completed before residual profiling began.

The new profile confirms 13.70 seconds in m14.11 island construction. About
28% of sampled top frames are the linear opcode-to-density lookup, with the
rest largely repeated partition pricing and candidate construction. The first
exact-semantics experiment will replace that lookup with a generated switch;
reusable interval costs and bounded search remain distinct follow-ups.

m15.7 still spends 80.7% in multi-normal density evaluation. A standalone
rvar-recorder probe failed to compile: the existing partials expose Eigen
arrays while the covariance density requires matrix additions. No production
change was made from that probe. A local native single-vector pullback using
the pinned Stan primitive operations is the bounded alternative to evaluate;
all activity masks, weighted reverses and refusal/domain cases need oracles.

Truncated-Poisson profiles put 56–57% in LCDF calls, with additional scalar
index/density dispatch. GEV spends 93.9% in a retained loop. Asymmetric Laplace
has 40 scalar islands per gradient (59.5%), plus scalar subtraction and index
operations. These fresh profiles replace the earlier pre-optimization profiles
as the next experiment's baseline. The preserved binaries and configuration
are under `/tmp/stanli-teaching-perf/baseline-ea4`; the research plan and failed
probe remain under the same evidence root. Further performance work is still
required; this checkpoint does not claim that every model beats CmdStan.

## Residual optimization after the v5 report checkpoint

The complete run `a224d12afe02ac98` (runtime/compiler `ea4d7e24`) remains
immutable: 192 complete comparisons, 178 lower Stanli CLI medians, 14 slower,
two caps and five pre-sampling failures. All baseline executables and their
hashes are retained in `/tmp/stanli-teaching-perf/baseline-ea4`. The report
checkpoint is `bba78e61`; subsequent source changes need new full-run evidence.

Preparation profiling of m14.11 found repeated linear opcode lookup and
allocation in partition pricing. Three changes preserve the original search
and register assignment: a generated opcode switch, flat temporary lookup
containers (never iterated for ordering), and exact additive cost prefix sums
while the graph is immutable. The pricing-cache ablation also disables the
prefix sums, so the existing full-graph equality fixture covers both paths.
All 65,536 opcode values are checked against the original name registry.
Three alternating preparation pairs gave 12.984 s baseline versus 11.393 s
with lookup; a separate matched experiment gave 10.330 s lookup versus
7.656 s with flat containers. Prefix sums improved that by about 2%.
A bounded confirmation used three identical-binary controls (median 0.297%
difference) and three new alternating pairs (1.825–2.327% gains). All passed
the predeclared threshold max(0.5%, twice median A/A noise); keep the cache.
Raw: `lookup-prep-ab.jsonl`, `flat-prep-ab.jsonl`, `prefix-confirmation.jsonl`
and `prefix-confirmation-summary.json` under `/tmp/stanli-teaching-perf`.

For single-vector covariance multi-normal, the recorder experiment could not
compile because its Eigen Array partials interface does not accept the Matrix
operations this density uses. The alternative uses the same primitive
factorization, solve/inverse, validation and partial arithmetic as pinned Stan
Math, retaining the computed partials in the existing scratch layout. It does
not change precision, Cholesky or vectorized-array kernels. A shape guard proves
the dimensions; active covariance keeps the explicit inverse, inactive
covariance the solve, preserving their distinct rounding. The standalone
weighted feasibility oracle passed 36,480 comparisons; integrated `test_mnc`
checks all eight activity masks, proportional/full forms, widths 1/2/3/8,
coincident, ill-conditioned and nonfinite cases, seven output weights, error
messages, and value-only/gradient reuse. Finite values and signed zeros require
bit equality; NaN payloads are not compared. The unchanged backward retains
its existing scatter and alias semantics.

Five alternating warm-gradient measurements (400 ms each, microseconds):

| Fixture | Before native covariance | Candidate | CmdStan |
|---|---:|---:|---:|
| ch15_m15_7 | 19.139 | 10.584 | 14.655 |
| ch14_m14_11 | 668.408 | 577.908 | 679.939 |
| ch14_m14_9 | 425.873 | 347.702 | 425.971 |
| s2_unstr (unaffected canary) | 7.219 | 7.204 | 6.473 |

Every candidate value and gradient equals the previous runtime bitwise at
the measured point; worst external relative difference is 4.41e-15. Raw:
`mn-gradient-ab.jsonl`. These are gradient measurements, not new sampling
medians. Full suites, numerical references and sampling remain integration
gates before publishing updated corpus results.

Truncated-Poisson profiling assigns 57% to scalar Poisson log-CDF calls. Their
integer outcomes were bound as length-one Eigen vectors, allocating temporary
arrays inside the unchanged special-function expression. A singleton-count,
scalar-rate call can instead use Stan Math's scalar overload. A 70-case probe
compared value, partial, connectivity and errors, including invalid rates,
zero/subnormal rates, negative counts, infinity and NaN: no differences.
Six alternating isolated measurements gave roughly 47–53 ns scalar versus
121–147 ns container. Only this proven distribution/shape is changed; all
other CDFs and broadcast shapes keep their original implementation. This
removes allocation around the same incomplete-gamma calculation; it does not
approximate it. `test_densities` now compares weighted kernel execution against
the prior container var overload. Raw: `poisson-scalar-probe.cpp` and `.log`.
Next gates: rebuild, focused oracles, paired actual-model measurements, all
CTests and references. Broader CDF singleton conversion remains untested.


### Residual preparation and categorical/matrix work (16 September)

The Poisson change passed the full 249-test suite, 316 reference-policy checks,
62 strict Rethinking references, and 456 R expectations without skips or warnings.
Five paired gradient windows gave upper-truncated Poisson 38.786 → 24.263 µs
(CmdStan 25.757), both-bounds 43.917 → 30.818 (31.339); an unaffected negative
binomial canary was 8.559 → 8.646 (8.724). All measured values/gradients were
bitwise equal to the preceding runtime. Raw: `poisson-gradient-ab.jsonl`.

A counterproposal to faster exhaustive partition pricing was to bound the
search itself. On m14.11, retaining the structured loop made preparation very
cheap but gradients about 3.5× slower; disabling liveness splits retained
bitwise values and cheap preparation. The implemented structural policy allows
max(65,536, four times span length) visited operations while pricing candidate
intervals. It always considers the whole span; unvisited leaves retain graph
operations. This changes the heuristic search, not density semantics, and is
ablatable with STANLI_ISLAND_PRICING_BUDGET=0. Three alternating pairs reduced
whole benchmark process time from 8.12–8.55 s to 0.719–0.734 s, with 400 ms
measurement and 200 ms warmup included; gradients stayed 583–588 versus 588–595
µs and bitwise equal. Tests exercise exhaustion and an unchanged small graph.
Raw: `gp-representation-probe.jsonl`, `pricing-budget-ab.jsonl`.

Audit correction: an ambiguous size_t test assertion initially failed to
compile, and a shell without `set -e` then ran a stale test executable. That
result was withdrawn. The assertion was corrected, and the fresh build/test
passed (`budget-corrected-test-build.log`, `budget-corrected-test.log`). The
316-reference run was fresh because its executable had relinked before that
failure. Subsequent multi-command build/test scripts stop on errors.

Scalar categorical-logit now keeps the primitive value/validation and applies
the selected-index contribution followed by scalar exponential normalization.
An Eigen packet-exp probe failed 253/32,144 exact comparisons; scalar std::exp
passed all of them. Integrated weighted tests also cover nonzero existing
adjoints, signed zeros, extremes and both proportional forms. Actual random
intercept categorical gradients fell 11.088 → 5.108 µs (CmdStan 7.567), bitwise
at the measured point (`categorical-chol-ab.jsonl`).

Categorical GLM can use the existing partial recorder with the unchanged Stan
Math probability function. The standalone probe passed 29,568 comparisons;
production tests cover scalar/array outcomes, empty arrays, one category,
rectangular designs, seven output weights and value-only/gradient reuse.
Scalar empty outcomes are excluded: the existing serialized layout carries no
outcome in that case. Five paired windows gave 1.460 → 1.206 µs (CmdStan 1.308),
bitwise (`cat-glm-ab.jsonl`). Other GLM kernels retain their existing paths.

Vectorized covariance/Cholesky densities now construct only the var or double
argument representation their activity mask needs. Precision keeps its original
bindings/scatter. Tests cover all eight masks, both densities and proportional
forms, shared/vectorized locations, empty/small shapes and weighted reuse.
Because the initial benefit was small, a predetermined confirmation used three
A/A and three A/B pairs. Median A/A noise was 0.797%; candidate gains were
1.482%, 4.053%, 2.546%, exceeding the declared median threshold 1.594%. Keep.
Raw: `mvt-confirmation.jsonl` and `mvt-confirmation-summary.json`.

The triangular self-product reverse now evaluates the same matrix callback,
including zero-initialized seeding and local adjoints before final scatter,
without rebuilding a var tape. A 5,488-comparison probe and integrated direct
kernel tests include rectangular matrices, masked upper entries, asymmetric
weights, overflow and prefilled adjoints. Five paired windows improved s2_unstr
6.874 → 6.100 µs (CmdStan 6.438), bitwise; the m14.9 canary was 342.735 → 343.962
(424.276). Raw: `mlt-probe.log`, `mlt-integration-test.log`, `mlt-ab.jsonl`.

Rejected/parked experiments: a native single-vector Cholesky kernel was correct
but did not accelerate the target, which uses vectorized arrays; removed. Lazy
construction of the adjoint call context produced sub-percent changes within
A/A noise; removed. Their patches remain local research artifacts. GEV and
asymmetric-Laplace profiles still show interpreter/control and dispatch cost;
changing loop policy alone did not establish parity. No sampling trajectory,
RNG, adaptation or numerical threshold was adjusted to improve timings.

These are fixed-point gradient and preparation results. A fresh complete
sampling sweep and full integration checks remain required; none are spliced
into the immutable a224d12afe02ac98 report checkpoint.


## Complete v6 sweep and the end-to-end parity gate

The user clarified that every tested model should reach end-to-end parity or
better; merely staying below the 3x cap is not success. Preserve preparation,
warmup, sampling, generated quantities and output in Stanli's measured CLI
boundary. Keep diagnostics alongside timings. Gradient improvements are
intermediate evidence only. No model-specific tuning, fingerprints, reference
edits or widened tolerances are authorized substitutes for this target.

Run `fd5e0ecacddc7047` finished all 199 fixtures on runtime `6e462c2e`: 193
complete comparisons, 187 lower Stanli medians, six slower, one capped and five
stopped before sampling. All 61 book calls completed and had lower Stanli CLI
medians, median CmdStan/Stanli 1.576; 49 passed the diagnostic screen in both.
The six slower fixtures were GEV, asymmetric Laplace, zero-inflated asymmetric
Laplace, mixture theta, educational Pareto, and cumulative ordinal. The cap was
`sw_re_negbin` seed 2. Its CmdStan reference chain finished unusually quickly
and the combined CmdStan fit had severe divergences, depth hits and poor R-hat;
that timing is not evidence of successful inference. The baseline remains
immutable, including the separately retained interrupted m12.6 attempt.

### Fixed-location GP arithmetic

A typed-input experiment resolved all three strict GP mismatches: keeping the
covariates double selects Stan Math's specialized exp-quad callback, whereas
promoting them to var selects a different reduction. The replay prototype
matched two models exactly and the third within 4.44e-16, but added overhead.
The native replacement retains the same blocked traversal, accumulates products
before scaling and uses the reference's scalar Eigen diagonal reduction.
A packetized diagonal prototype failed exact tests by a few ULP and was
replaced; no tolerance was relaxed. Tests now use the proper double-location
oracle, all eight activity masks, empty/repeated points, block boundaries,
asymmetric weights, extreme scales, and existing nonzero input adjoints.
Active-location replay remains unchanged.

Five alternating gradient pairs, 200 ms warmup and 400 ms measurement, gave
CmdStan/Stanli ratios 1.237 (`sw_gp`), 1.310 (`i320_gp_expquad`), 1.311
(`s2_gp_by_gr`) and 1.289 (m14.8 canary). These are warm-gradient ratios.
The three previously failing fixtures match every recorded value at all three
CmdStan reference points exactly; the four-model replay compares 900 values,
with the m14.8 canary's largest scaled error 3.00e-16.

Separate four-seed 1000+1000 sampling checks, with the original 3x/900s cap,
produce complete results for all three GP fixtures: median Stanli/CmdStan
seconds 1.033/1.233, 0.623/0.860 and 0.545/0.814. Their CmdStan/Stanli CLI
ratios are 1.194, 1.381 and 1.492. Both engines retain diagnostic flags on
these models, so completion is not claimed as reliable inference. These
checks are not inserted into the fixed-point-gated v6 aggregate.

Inverse Gaussian also samples in both engines: 0.0248/0.0495 seconds, all four
seeds complete. Both have many divergences. Its original fixed test point is
outside the model's domain in both engines; this is not missing sampling
support. A generic valid-point benchmark policy still needs separate design
and verification, retaining the original refusal evidence.

### Nullary reader fix and remaining work

MIR arity validation rejected valid log2()/log10() constants already supported
by execution. Reuse shared nullary recognition; test actual decoding and
execution, unary overloads and malformed arities. COM-Poisson now reaches the
existing dynamic-loop limitation instead of failing at log2. Prefer/force
structured-loop probes additionally reveal a runtime integer outcome that
native density lowering expects to be compile-time data. COM-Poisson is not
yet fixed.

Current integration: 249/249 tests, 316/316 existing-policy reference checks,
and 456 R expectations with no warnings, failures or skips. The existing
cross-platform ill-conditioning policies have not been changed on the basis
of one machine. Initial reference execution in the separate build found an
omitted embedded-compiler configuration; it was corrected before the recorded
numerical and complete-run checks. The original measured build is untouched.

Artifacts under `/tmp/stanli-teaching-perf`: `gp-ordered-pairs.json`,
`gp-ordered-identity.json`, `gp-ordered-candidate.patch`,
`gp-ordered-three-point-embedded.log`, `blocker-sampling-v7/`, and
`residual-v7-{ctest,refs,r-tests}.log`. The separate candidate build is
`build-teaching-residual`. Simple loop/carving/liveness policy probes are
retained in `v7-representation-probes.json`; none resolves the large remaining
GEV/ALD deficits. Remaining performance targets and COM-Poisson stay open.


## Practical end-to-end target (user revision)

The user accepts CmdStan/Stanli >= 0.8 for complete CLI sampling if the last
20% of speed would require disproportionate architectural work. This means
Stanli elapsed time <= 1.25 times CmdStan, not a 3x-cap pass. Prefer small,
general, measured changes. Keep numerical correctness and sampling support
as independent requirements. Do not add a large subsystem just to close a
small residual timing gap. No change to reference data, tolerance, adaptation,
RNG, benchmark inputs or the experiment's 3x/900s cap.

Against the immutable v6 measurements, four completed models remain below
0.8: s2_gev (0.420), sw_asymlaplace (0.677), s2_zi_asymlaplace (0.700),
and s2_mixture_theta (0.771). Educational Pareto (0.939) and cumulative
ordinal (0.957) already meet the revised target. The mixture and the capped
sw_re_negbin have severe diagnostic flags in both engines, so investigate
trajectory work separately from execution throughput. All 62 Rethinking
fixtures already exceeded 1.0 in this sweep. New GP fixes retain their
separate source-identified evidence until a fresh complete sweep.

The small native-conditional COM-Poisson prototype did not change the
failure: runtime-control still requires its dynamic integer k at compile
time. It was removed from production sources; the isolated patch and probe
log remain under /tmp/stanli-teaching-perf/com-native-branch-*. No support
claim follows from this attempt. A broader program/lowering change needs
its own bounded evaluator; the failed prototype is not shipped.

### Small-change follow-up and report checkpoint

A 20-line structured-sequence flattening experiment preserved the tested
values exactly and passed the structured-loop suite but did not meet its
predeclared 5% gradient benefit criterion. Five counterbalanced pairs gave
baseline/candidate microseconds 13.164/13.268 (GEV), 5.920/5.921 (ALD),
6.240/6.180 (zero-inflated ALD), and 2.689/2.651 (m14.8 canary). No meaningful
win was established; the prototype was removed. The raw plan, patch, identity,
measurements and logs remain under `/tmp/stanli-teaching-perf/sequence-*`.
No runtime change is integrated from this attempt.

The published teaching tables and four-page McElreath PDF now use the complete
v6 checkpoint (`fd5e0ecacddc7047`, runtime `6e462c2e`). All PDF pages were
rendered and visually inspected. The 550,455,487-byte raw archive is retained
locally, excluded from Git, with SHA256
`30840d8b8ed7ae4734ec3d69f41fff54bad51c91d9f4cf2227e00667182125ad`.
Interrupted partial evidence remains separately labeled inside it. Published
text logs normalize whitespace; the archive retains their original bytes.

The GP/inverse-Gaussian checks are published separately under
`output/teaching-performance/followup-4db5dca2/`. Applying the retained measured
patch to its recorded baseline reconstructs exactly the two runtime files in
commit `4db5dca2`; source equivalence and executable identity are included.
These results do not alter v6's counts. PR CI at `b5b61a80` passes its Linux
runtime R and compiler checks; the earlier full platform/sanitizer run on
`6e462c2e` remains the full-sweep platform evidence. Remaining support and
performance gaps are open, so the draft PR is not ready to claim full parity.

## Post-merge COM-Poisson feasibility (16 September)

PR #371 merged as `36310458`; report wording cleanup #375 merged as `e27bad19`.
All PR checks passed. The earlier full platform run also passed Linux/macOS/
Windows R and sanitizers. The report is public in the repository, with neutral
wording, per-model speedups and absolute numerical differences.

On a clean Release rebuild of `e27bad19`, a small early-return function with a
parameter-controlled while, `terms[n] = z*n`, and `log_sum_exp(terms[1:n])`
reproduces COM-Poisson's integer-constant refusal. Fixed-prefix and scalar-read
variants also fail: the first refusal is the runtime vector write, before the
reduction. Without the early return, native structured lowering instead lacks
an integer range proof for the prefix after the loop.

A separate 60-line feasibility patch implements checked dynamic scalar vector
writes and a fused dynamic-range log-sum-exp using existing Stan Math var
replay. The tiny function evaluates to 1.6425355294551629 with derivative
2.624647182103895 at z=0.1, matching an independent closed-form calculation to
within 1e-14. A counter-only variant and dynamic scalar-read variant also work.
The actual COM-Poisson fixture then advances to unsupported `Modulo__`, at all
three reference points. A fixed-prefix UDF variant additionally exposes
unsupported full-vector indexing in the register fallback.

This is feasibility evidence, not a shipped support fix. The patch was removed
from production source at the predeclared next missing capability, and the
checker was rebuilt from restored source. No timings were measured, no full
model numerical comparison succeeded, and no published counts or reference
policies changed. [Issue #376](https://github.com/seantalts/stanli/issues/376)
contains the reproducer, findings and next validation gates. Local artifacts:
`/tmp/stanli-teaching-perf/com-minimal/` (plan, variants, patch, candidate checker,
logs, closed-form oracle and executable hash). The next bounded experiment is
integer remainder and full-vector indexing semantics, followed by the complete
model oracle; do not integrate the prototype without adversarial, replay,
consumer and end-to-end checks. Frozen v6 inputs and binaries remain untouched.

## Renewed educational/brms gap work

The user explicitly requested continued performance and numerical work. The
COM-Poisson candidate now handles checked runtime vector element writes,
scalar log-sum-exp over a runtime range, integer remainder through Stan Math,
and identity indexing left by index composition. Native adjoints refuse the
new dynamic operations; existing var replay supplies their derivatives.
Generic compiler tests cover row/column vectors, aliased RHS, changing indices
and ranges, empty ranges, invalid bounds, unused NaN tail values, compaction,
integer sign/boundary cases, and independent Stan Math derivatives.

COM-Poisson matches its unchanged independent CmdStan density/gradient reference
at all three points, maximum absolute difference 2.84e-14. Independently rebuilt
write-array output also matches. Validation: 249 runtime tests, 316 existing-
policy reference checks (1,008,755 values), and 456 R expectations, no failures,
warnings or skips. Removing the former compilation-gap entry does not remove
any numerical/domain policy or imply sampling-speed parity.

Fresh four-seed 1000+1000 CLI measurements retain the 3x/900-second cap. CmdStan
completed all seeds (median 1.1000 s); Stanli hit the relative cap on all four,
so there is no Stanli median. This is improved compilation coverage, not a
completed performance fix. The separate evidence and exact source patch are
in `/tmp/stanli-teaching-perf/gaps-v8/com-sampling/`; v6 remains unchanged.
A diagnostic profile points to Stan Math variable allocation during replay,
including large constant fills, as a substantial cost. Next bounded test:
represent a uniform constant range as a broadcast, preserving every double
bit pattern and derivative, then compare gradients and capped full sampling.
ALD/GEV dispatch costs remain a separate performance beam.
