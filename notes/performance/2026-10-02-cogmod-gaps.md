# cogmod families: gradient gaps and their causes

Scope: [issue #422](https://github.com/seantalts/stanli/issues/422) reported
that cogmod's brms custom families are slow under stanli when their densities
branch on parameters. This note records the 21 families measured against
CmdStan, the causes found, the fixes, and what remains open.

## Corpus and method

The programs are `brms::make_stancode()` output with cogmod's stanvars, from
cogmod dev at `732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732`, about 2000 simulated
observations each. `lnr_bench` reproduces the issue's LNR setup (speed_acc
participants 1-3, N=4620, `sigmabias = 0`). These models, at a smaller N,
are now checked in at `tests/cogmod` (MIT; see its README).

Gradients were timed with `bench_grad --timed` and CmdStan's equivalent from
`tools/bench_cmdstan_grad.cpp` at `-O3`, on an idle Apple M-series laptop, at
the fixed point `bench_grad` uses. Gradients agree with CmdStan to within
8e-15 relative on every family that runs in both.

The MIR must come from the embedded compiler (`stanli_check
--dump-passes=mir`), which is what Python and R users get. Since #403 that
pipeline runs O1 without stanc's partial evaluation, so branches on constants
reach stanli unfolded. Plain `stanc --O1` MIR hid two of the problems below
and overstated others.

## Results

These are fixed-point gradient timings: one parameter point, evaluated
repeatedly. They overstate the structured loop. Its replay is specialized to
the branch decisions it recorded, and under sampling several families
(exgaussian, geg, lognormal, exwald, both LNR programs) change branch on most
gradients and re-record the whole loop each time, about 10x slower than
per-observation regions. See the sampling measurements in
[the replay plan](../execution/2026-10-02-loop-replay-regrouping-plan.md).

Per gradient in microseconds. "main" is `d4a88756`, which includes #423.
"branch" adds the commits described below.

| family | main | branch | CmdStan | branch / CmdStan |
| --- | ---: | ---: | ---: | ---: |
| betagate | 777 | 768 | 542 | 1.42 |
| geg | 859 | 864 | 634 | 1.36 |
| invgaussian | fails | 2038 | 1503 | 1.36 |
| bisa | 246 | 252 | 190 | 1.33 |
| exgaussian | 480 | 477 | 364 | 1.31 |
| lnr | 2491 | 381 | 292 | 1.30 |
| weibull | 253 | 247 | 192 | 1.29 |
| exwald | 329 | 327 | 256 | 1.28 |
| choco | 969 | 963 | 760 | 1.27 |
| gamma | 253 | 253 | 199 | 1.27 |
| lognormal | 368 | 364 | 287 | 1.27 |
| invweibull | 253 | 255 | 203 | 1.26 |
| rdm | 339 | 343 | 272 | 1.26 |
| loggamma | 254 | 252 | 204 | 1.24 |
| logstudent | 258 | 255 | 207 | 1.23 |
| invgamma | 256 | 255 | 212 | 1.20 |
| logweibull | 209 | 209 | 178 | 1.17 |
| lba2 | 363 | 360 | 328 | 1.10 |
| lba1 | 326 | 328 | 301 | 1.09 |
| betadiscrete | 18818 | 18916 | 18969 | 1.00 |
| lnr_bench | 7681 | 628 | 644 | 0.98 |
| ddm | fails | 400601 | 399770 | 1.00 |

Preparation is 0.01 to 0.04 s for every family except betadiscrete (0.13 s).
On main, `lnr_bench` took 5.2 s and `lnr` 2.3 s. Both LNR programs now match
CmdStan's log density and gradient bitwise, as does ddm; invgaussian is within
1 ULP.

## Causes found and fixed

Merged in #423:

- **Branchy regions skipped register compaction.** Any runtime-control region
  containing a jump kept registers for the whole data vector it indexed, so a
  per-observation branch cost O(N) per region and O(N^2) per gradient.
- **Packed region inputs copied whole vectors.** A region with more than six
  live-ins concatenated entire slots. LNR concatenated two 4620-element vectors
  per observation.
- **`log_mix` and comparisons used as values** did not compile inside regions.
- **Bounded specialization unrolled loops into one region per iteration**
  where the structured loop compiles the body once. The trial now refuses in
  that case.

On this branch:

- **Constants did not fold inside regions.** A transformed parameter fixed at
  a constant reached a region as an ordinary input, so every arm it selected
  between was compiled. In `lnr_bench`, `sigmabias = 0` left 1913 instructions
  and 93 kernel calls per observation where 181 and 5 are reachable. The region
  compiler is now seeded with known scalar constants: 3.5x faster on
  `lnr_bench`.
- **Guard-clause returns failed to compile in functions stanc did not
  inline.** invgaussian exceeds the O1 structural budget, so the embedded
  compiler falls back to O0 and stanli inlines the functions itself. A guard
  such as `if (mu <= 0) return negative_infinity();` followed by more code
  failed with "runtime-control region: return inside runtime control without a
  function scope". Such bodies are now rewritten into stanc's single-exit form.
- **The structured loop refused stanc's inlined container returns.** The
  inliner declares a vector-returning function's result zero-length and
  assigns it in several arms. The structured compiler also lowered
  statements after an unconditional `break`, which is where a constant-folded
  LNR branch left that code. It now skips unreachable statements and sizes
  such a local from the static shape its assignments agree on, so both LNR
  programs use one structured loop instead of one region per observation.
- **ddm did not compile.** Its density calls `wiener_lpdf` with five and
  seven parameters plus a precision argument; stanli had only the
  four-parameter kernel and its reader rejected six arguments. A packed kernel
  now covers those forms, adding 0.94 MB to the stripped shared library.

## Open

- **Structured-loop dispatch on the 1.1 to 1.4x families.** A sampled gamma
  gradient spends about half its time in `structured_loop_forward` and
  `structured_loop_backward` themselves, and about a sixth in the math
  functions CmdStan also calls. Gamma runs in replay mode: each gradient
  replays about 48,000 recorded scalar kernel calls, 24 per observation, plus
  12,000 branch guards and 14,000 backward instructions. Every observation
  repeats the same instruction pattern, so batching each instruction across
  observations that took the same path would replace per-element dispatch with
  vector kernel calls. That is a design question, not yet measured beyond this
  ceiling.
