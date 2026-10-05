# Numerics versus speed: what CmdStan matching costs, and a possible fast mode

Status: research record, 2026-10-05. Nothing here changes behaviour. It
collects every place where stanli gives up speed to compute the same numbers
as CmdStan, with measurements, so that a future opt-in fast mode can be
designed from evidence. Data and scripts are in
[data/2026-10-05-numerics-vs-speed/](data/2026-10-05-numerics-vs-speed/).

## How this came up

The corpus benchmark was rerun on 2026-10-04 (main `bd52ed4a`, 342 of 352
models) and compared with the published run of 2026-09-21 (runtime
`2c9d67b9`). CmdStan's own times were unchanged (median 1.03x), but stanli's
per-gradient time was more than 10% slower on 97 of 313 shared models, up to
5x: aalto_poisson_hurdle 2305 -> 12413 ns, M0_model 1062 -> 4069,
ch12_m12_3 1064 -> 4228, ch11_m11_5 6631 -> 19702, and many rethinking
chapter 11-13 multilevel models at 0.33-0.44x. v0.18.1 was already slow, and
the step is #403 (`f1face39`, "Match CmdStan numerics and speed up brms
models", merged 2026-09-22), which made the 124 brms fixtures agree with
CmdStan within 10 ULP.

Builds just before and after #403, with private copies of the embedded
compiler, reproduce the published and current times. Crossing compiler and
runtime between the two builds, and switching each change off with the
diagnostic toggles in `diagnostic-toggles.patch`, separates three mechanisms.

## The three mechanisms

### 1. Active duplicates are no longer merged (runtime, `runtime/src/cse.cpp`)

Before #403, common-subexpression elimination merged identical ops even when
their inputs depend on parameters. The merged op ran one backward with the
duplicates' output adjoints added first. #403 keeps every duplicate: they
share the forward value (`primal=sN`) but each runs its own backward, because
adding the seeds before a nonlinear pullback reassociates the sum.

- Cost: the 3-5x cases. On M0_model the backward of BERNOULLI_LPMF,
  BINOMIAL_LPMF, ADD and LSE2 went from about 3 ms to 280 ms per 20000
  gradients. Switching the merge back on restores aalto_poisson_hurdle
  (12109 -> 2900 ns), M0_model (3907 -> 1046), ch12_m12_3 (4136 -> 1157),
  ch11_m11_5 (19774 -> 6583), and takes the slowed set's median slowdown from
  1.23x to 1.06x.
- What it protects: five brms fixtures exceed 10 ULP when merged:
  s2_car 16, s2_mo_simo_prior 128, s2_zi_asymlaplace 18, sw_mono 418,
  sw_skewnormal 60.
- Plan: recover the speed exactly. The duplicates' partials are identical,
  so one backward can compute them once and apply each duplicate's seed in
  the original order (branch `perf/cse-shared-primal`, in progress).
- Fast mode: merging is the natural fast-mode behaviour if the exact
  recovery falls short anywhere.

### 2. Fusion declined when a shared parameter is read twice (runtime, `reroll.cpp`, `partition.cpp`)

#403 declines widening or partitioning a repeated body into vector kernels
when the same active scalar is read at more than one position per iteration.
(Main later exempted reads by values computed once per evaluation.)

- Cost: about 2x on six models: s2_discrete_weibull 1801 -> 3601 ns,
  s2_hurdle_negbin 3370 -> 6655, Mtbh_model 14638 -> 20390, sw_hurdle_gamma
  1539 -> 2190, and two more.
- What it protects: s2_discrete_weibull (14 ULP) and s2_hurdle_negbin
  (34 ULP).
- Accuracy against a 70-digit reference (`hp_reference.py`,
  `compare_ulps.py`; the reference agrees with CmdStan's recorded values
  within 13.2 ULP on all 27 values and with itself at 110 digits): only the
  log-shape gradient differs between the paths, and the fused path is less
  accurate.

  | model | CmdStan, max ULP | current, max ULP | fused, max ULP |
  | --- | ---: | ---: | ---: |
  | s2_discrete_weibull | 10.40 | 10.40 | 14.49 |
  | s2_hurdle_negbin | 13.24 | 13.24 | 20.76 |

  Per point, the fused path is worse at all three points of
  s2_discrete_weibull, and better at two and worse at one of
  s2_hurdle_negbin.
- Why: for discrete Weibull, observation n contributes
  `c1_n = [A log mu / (A - B)] p1 log y` (negative) through `y^shape` and
  `c2_n = [-B log mu / (A - B)] p2 log(y + 1)` (positive) through
  `(y + 1)^shape`, with `A = mu^p1`, `B = mu^p2`. The scalar reverse pass
  adds them interleaved, last observation first: `c2_N, c1_N, c2_{N-1},
  c1_{N-1}, ...`, so each pair cancels before reaching the running sum. The
  fused path runs two vector `pow` backwards (`pow_bwd` in
  `runtime/kernels/eltwise_expr.cpp`), adding all `c2_n` for n = 1..N and then
  all `c1_n`. The running sum first grows to the size of the positive half,
  and rounding error scales with it. Same terms, same number of roundings,
  worse order.
- Decision (2026-10-05): keep the check. The scalar order is the more
  accurate one here, not only the CmdStan one.
- Fast mode: fusion with a pairwise or per-observation-pre-summed reduction
  could recover both speed and accuracy; plain fusion trades accuracy for
  about 2x on these shapes.

### 3. stanc3 partial evaluation off (compiler, `compiler/ocaml/stanli_pipeline.ml`)

#403 runs stanc3's O1 pipeline with `partial_evaluation = false` (and
`preserve_stability = true`). Partial evaluation rewrites, for example,
`beta[1] + beta[2] * mom_iq` into `fma(beta[2], mom_iq, beta[1])`, which rounds
differently from CmdStan's default build.

- Cost: 10-20% on about 25 models: mesquite 476 -> 568 ns, kidscore_momiq
  1598 -> 1864, dogs 6585 -> 8507, radon_pooled 47425 -> 55671,
  logearn_interaction 7568 -> 9006, and others in the kidscore, logearn,
  earn_height, nes, arma11 and ch14 families. Compiler-only: it shows with the
  old runtime too. `preserve_stability` and the inline-args patch cost nothing.
- What it protects: s2_gev (16 ULP), s2_me2_nomecor (53), sw_me (16).
- Decision (2026-10-05): keep it off. CmdStan's default is `-O0`, which never
  runs partial evaluation, so this matches what most Stan users run; it is
  not a regression against CmdStan.
- Accuracy against a 70-digit reference (scripts in
  `data/2026-10-05-numerics-vs-speed/partial-evaluation/`, run from a checkout
  with two `stanli_check` builds, partial evaluation off and on): roughly
  neutral, not strictly better.
  - s2_me2_nomecor: better. Its gate failure is generated quantities only;
    worst error 0.5 ULP with partial evaluation against about 53 for CmdStan
    and the current build. Over 200 random points it is never clearly worse.
  - sw_me: mixed, leaning better. Generated quantities improve; one
    near-zero gradient component is 13.4 ULP off instead of 2.2 (about 2e-17
    absolute).
  - s2_gev: worse at the three recorded points (worst 326 ULP against
    CmdStan's 154); over 200 random points a statistical tie on gradients
    (better at 93 points, worse at 91), with a larger worst log-density error
    (250 ULP against 95).
  - Corpus-wide, 23 models change, all by at most 5e-14 scaled error. The
    151x151 GP models ch14_m14_10 and ch14_m14_11 get slightly more accurate;
    one gradient component of ch15_m15_8 gets slightly worse (6.6e-15
    absolute).
- Fast mode: turning partial evaluation back on is the simplest fast-mode
  switch (about 10-20% on linear predictors), and accuracy-neutral overall.

## Other speed-for-numerics trades already measured

- **Data-class specialization** (grouping observations by low-cardinality
  integer data such as `dec[n]` and compiling one body per group): 1.03x on
  lnr_bench under sampling, but it changes gradient order (up to 19 ULP on a
  test fixture) and which observation reports an error first. Not shipped;
  branch `feat/map-dataclass`, local only.
- **Dense blending under partial masks** in lane execution: no consistent win
  on arm64 (NEON is two doubles wide).
- **x86-64 instruction sets** (GitHub runner, AMD EPYC 7763, all 350 timeable
  models, baseline vs `-march=x86-64-v3` with and without FMA;
  `isa-timing-summary.txt`, `isa-variant-diff.tsv`): v3 median 1.037x, geomean
  1.07x, 14 models at least 1.5x faster (nn_rbm1bJ100 2.2x, the wells and nes
  families about 2x), 36 models more than 5% slower (ch12_m12_4 0.70x,
  i319_gauss_re 0.74x). The CmdStan corpus replay passes 308 of 352 for v3
  (311 without FMA) against 352 for baseline: 41 brms 10-ULP gates and three
  ill-conditioned GP models (scaled error up to 3.6e-7). FMA is worth about
  0.5-1%. AVX-512 was not available on the runner.

## A fast mode, if we build one

The pieces above combine into an opt-in mode (a flag such as `--fast-math`
or `STANLI_FAST_MATH=1`) that would:

- merge active duplicates (only if the exact shared-primal backward leaves a
  gap);
- allow fusion over shared parameters, ideally with a pairwise reduction so
  accuracy does not suffer;
- run stanc3 partial evaluation (fma contraction of linear predictors);
- possibly select an x86-64-v3 runtime where the CPU supports it.

What it would give up: agreement with CmdStan within the brms 10-ULP gates,
bitwise agreement between CPU classes, and, for some shapes (mechanism 2),
accuracy against the true value. Its tests would compare against a
high-precision reference with a scaled-error bound, not against CmdStan's
ULPs.

## Plan

We intend to build the fast mode one day, as a collection of speedups like
the ones above. Until then this note is where candidates go: whenever a
change is rejected or held back because it moves results away from CmdStan
or the high-precision reference, add it under "Candidates" with its measured
speedup, what it changes numerically, and where its code or toggle lives.

Before the mode ships it needs its own evidence, separate from the default
build's:

- Benchmarks: the corpus benchmark in fast mode, paired against the default
  mode and against CmdStan, plus the sampling benchmark once it exists, so
  each candidate's contribution is visible per model.
- Numerics: every corpus model at the recorded evaluation points against a
  high-precision reference (as `hp_reference.py` does for two models), with a
  scaled-error bound in place of the 10-ULP CmdStan gates, and a report of
  where fast mode is less accurate than the default and by how much.
- Sampling checks: posterior agreement with the default mode (means,
  intervals, R-hat, ESS per gradient) on the corpus models that have
  reference posteriors.

### Candidates

| candidate | measured speedup | numerics change | where |
| --- | --- | --- | --- |
| Merge active duplicates in CSE | up to 5x (aalto_poisson_hurdle, M0_model, rethinking ch11-13); only if the exact shared-primal backward leaves a gap | seeds summed before the pullback; 5 brms fixtures 16-418 ULP from CmdStan | `X_NO_CSE_ACTIVE` in `diagnostic-toggles.patch` |
| Fuse over shared parameters | about 2x on 6 models | different summation order; less accurate on discrete Weibull and hurdle negbin unless the reduction is pairwise or pre-summed per observation | `fusion-toggles.patch` |
| stanc3 partial evaluation | 10-20% on about 25 models | `fma` contraction of linear predictors; 3 brms fixtures 16-53 ULP from CmdStan; accuracy-neutral against a 70-digit reference (better on s2_me2_nomecor, worse on s2_gev at its recorded points) | `partial_evaluation` in `compiler/ocaml/stanli_pipeline.ml` |
| Data-class specialization | 1.03x on lnr_bench | gradient order across groups (up to 19 ULP); first-error observation changes | branch `feat/map-dataclass` |
| x86-64-v3 runtime | median 1.04x, up to 2.2x; 36 models slower | 44 of 352 replay failures; CPU-dependent results | scratch branch `bench/avx2` |
