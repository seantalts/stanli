# Issue 374: matched 2.40 refresh and GEV profile

The current comparison narrows [issue #374](https://github.com/seantalts/stanli/issues/374)
to three credible execution-cost targets: GEV and the two asymmetric-Laplace
fixtures. Twelve of the thirteen selected models complete every seed. The
remaining cap involves a pathological negative-binomial chain in the reference.
The five formerly capped ordinal fixtures now have lower Stanli CLI medians.
This investigation changes no compiler, runtime, fixtures, or numerical gates.

The [triage note](2026-09-21-issue-374-triage.md) contains the historical evidence.
The [compressed evidence packet](2026-09-21-issue-374-evidence.json.gz) retains
all primary pairs, sampling events including the timeout, numerical answers,
phase trials, diagnostics, profile summaries, commands, and artifact hashes.
It is UTF-8 JSON compressed with gzip. Raw logs and executable helpers remain
in the local ignored directory `.cache/issue374-20260921/`.

## Frozen identity and protocol

- Stanli: `45ea5cca1e4f583e57963438a0a3994841c5ab69`, clean tracked sources,
  synchronized with fetched `origin/HEAD` before building.
- CmdStan: isolated checkout `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`
  (2.40.0), deliberately pinned rather than using the older shared installation.
- Both use Stan `a6806ef8477a7b5f65b27449ec33528c162bc024` and Math
  `5252d51d47c1d5e78005fc043ad996fad6dd8da8`. Stanli's existing CVODES
  quadrature-initialization patch is recorded; none of these models uses it.
- stanc source pin: `d58446e631b02cacc5355e373defc6092a684554`.
  The executable's version string remains `v2.39.0-210-gd58446e`; the source pin
  and artifact hashes, rather than that label, establish identity. The native
  embedded compiler, vectorizing probe, and stock reference compiler have
  recorded provenance. Binaries stayed unchanged throughout the sweep.
- Apple M3 Ultra, 96 GiB, 32 logical CPUs, macOS 26.6.2 arm64; fresh Release
  build, `-O3`, `-ffp-contract=off`, Stanli threads enabled but one runtime thread.
  Other host work was present; this was not an isolated machine.
- Unmodified [corpus v3 protocol](../../docs/benchmark-protocol.md): six
  alternating gradient pairs, 200 ms warmup and 250 ms measurement per process;
  complete density/gradient comparison before accepting each pair. Preparation
  from existing MIR is separate from source compilation.
- Full CLI: seeds 1–4, 1,000 warmup + 1,000 retained draws, default initialization,
  `adapt_delta=0.8`, depth 10. CmdStan runs first for each seed; Stanli gets
  `min(3 × reference wall time, 900 s)`. This fixed order is a limitation.
  Source preparation, generated quantities, and CSV formatting count toward
  Stanli CLI time; ahead-of-time CmdStan compilation is excluded. Default CSV
  precision is 17 significant digits for Stanli and eight for CmdStan.

Eleven targets are the eight originally capped fixtures plus GEV, mixture
theta, and cumulative ordinal. `sw_cumulative_cs` and Eight Schools are controls.
All belong to the shared corpus; twelve have brms provenance and Eight Schools
has posteriordb provenance. This is a targeted refresh, not a new full-corpus run.

## Current measurements

Times below are medians. Gradient ratios are medians of within-pair
**CmdStan/Stanli** ratios; the accompanying number is MAD, not a confidence
interval. CLI ratios divide the two four-seed medians. Ratios above one favor
Stanli. A small difference, particularly cumulative ordinal, is not an
established performance advantage or regression.

| Model | Gradient µs, Stanli / CmdStan | Paired ratio (MAD) | CLI ms, Stanli / CmdStan | CLI ratio |
| --- | ---: | ---: | ---: | ---: |
| `eight_schools_noncentered` | 0.226 / 0.420 | 1.898 (0.049) | 21.63 / 32.64 | 1.509 |
| `i320_sratio_cs` | 78.935 / 109.159 | 1.384 (0.041) | 1739.99 / 2795.65 | 1.607 |
| `i320_sratio_plain` | 60.804 / 76.458 | 1.254 (0.061) | 1037.93 / 1518.05 | 1.463 |
| `s2_gev` | 6.962 / 5.011 | 0.698 (0.021) | 124.75 / 96.75 | 0.776 |
| `s2_mixture_theta` | 6.761 / 8.999 | 1.309 (0.032) | 1407.85 / 1632.64 | 1.160 |
| `s2_zi_asymlaplace` | 4.869 / 4.532 | 0.911 (0.014) | 167.33 / 144.57 | 0.864 |
| `sw_asymlaplace` | 4.668 / 3.324 | 0.716 (0.009) | 135.73 / 113.25 | 0.834 |
| `sw_cratio` | 58.354 / 74.976 | 1.284 (0.042) | 776.74 / 1062.33 | 1.368 |
| `sw_cratio_cs` | 76.819 / 110.264 | 1.404 (0.027) | 1288.45 / 2032.48 | 1.577 |
| `sw_cumulative` | 22.179 / 21.957 | 0.984 (0.013) | 314.17 / 309.99 | 0.987 |
| `sw_cumulative_cs` | 53.575 / 64.855 | 1.206 (0.027) | 1278.28 / 1513.80 | 1.184 |
| `sw_re_negbin` | 2.122 / 2.726 | 1.292 (0.006) | censored / 4943.74 | — |
| `sw_sratio` | 58.985 / 78.449 | 1.267 (0.028) | 774.84 / 1086.46 | 1.402 |

GEV takes about 29% longer end to end. Asymmetric Laplace takes 20% longer and
its zero-inflated variant 16% longer. GEV's first CLI pair was much slower in
both engines (542/419 ms); all seeds are retained, including this observation.
One run per seed cannot isolate filesystem/cache effects from other host noise.

For negative binomial, CmdStan seed 2 finishes in 0.203444 s with **982 of
1,000 retained draws divergent**. Stanli times out at a 0.610331 s cap
(observed process duration 0.622649 s). Its other seeds finish in 3.679,
3.855, and 3.018 s. There is no aggregate Stanli time over those survivors.

## Sampling work and diagnostic limits

GEV and both Laplace fixtures have zero divergences and depth hits in both
engines. Across them the largest R-hat is 1.0062. GEV's minimum bulk ESS is
2,248 for Stanli and 2,267 for CmdStan. The five formerly capped ordinal
fixtures also have clear screens in both engines.

Mixture theta's lower wall time is not evidence of faster useful inference:
Stanli has 12 divergences, 135 depth hits, R-hat 2.573, and minimum bulk ESS
4.78; CmdStan has 1,333 divergences, 198 depth hits, R-hat 2.150, and minimum
bulk ESS 5.23. Negative binomial's complete CmdStan run has 1,309 divergences,
2,691 depth hits, R-hat 35.87, and minimum bulk ESS 4.03. The cumulative-CS
control also has divergences (189 Stanli / 165 CmdStan), and Eight Schools
has zero / two. These timings are not ESS-per-second claims.

GEV retained leapfrog counts, seeds 1–4, are `6090, 5840, 5476, 5554` for
Stanli and `5470, 5216, 6652, 6182` for CmdStan. Stanli's total gradient
evaluations, including warmup and initialization, are `14888, 14582, 14211,
14166`. Retained leapfrogs alone do not describe adaptation work; CmdStan's
total gradient count was not instrumented. The independently slower fixed-point
gradients nevertheless identify a real execution-cost target.

## Preparation, first gradient, and memory

Five fresh probe processes per model separate MIR/data loading, lowering,
binding, and the first gradient. The packet retains all thirteen models and
all phase trials. Four models also have four-seed CLI timing/RSS reruns.
Every rerun reproduced its engine's primary CSV header and draw bytes exactly,
excluding comments. These diagnostic reruns do not replace primary timings.

| Model | MIR preparation ms | First gradient µs | CLI preparation ms | CLI peak RSS MiB, Stanli / CmdStan |
| --- | ---: | ---: | ---: | ---: |
| GEV | 0.887 | 123.5 | 6.662 | 16.45 / 4.25 |
| Asymmetric Laplace | 1.510 | 17.1 | 6.524 | 16.33 / 4.17 |
| Zero-inflated asymmetric Laplace | 4.520 | 23.6 | 9.916 | 17.10 / 4.22 |
| Eight Schools | 0.376 | 9.3 | 2.559 | 13.84 / 4.11 |

GEV's MIR preparation splits into approximately 0.175 ms loading, 0.701 ms
lowering, and 0.009 ms binding. Its first gradient is too short to explain
the roughly 28 ms CLI gap. The full CLI preparation timer includes source
compilation and runtime preparation; source compilation alone was not isolated.
The CLI sampling timer includes generated quantities and CSV formatting;
`output_s` measures only spool copying and final flushing. It cannot establish
that all output work is negligible.

RSS is whole-process high-water memory from macOS `/usr/bin/time -l`, including
the source compiler in Stanli's CLI. It is not retained executor memory and
cannot be subtracted to estimate individual components. The MIR-only GEV probe
peaks at 6.52 MiB. The full CLI memory gap is real baseline evidence, not a
measured regression caused by this investigation. Download size, installation
size, and packaged-artifact startup were not measured here.

## Numerical fidelity

All primary gradient pairs pass the existing `1e-9` scaled-error gate.
Separately, all 39 model/point combinations pass density/full-gradient and
write-array checks against a freshly compiled independent reference. Output
names agree, and the benchmark MIR path is bitwise identical to the shipped
source-compiler path, including signed zeros.

- Density/gradient: **252/282 values within 10 ULP (89.4%)**; maximum 5,861 ULP,
  maximum scaled error `4.4121e-13`.
- Per-draw outputs: **427/429 within 10 ULP (99.5%)**; maximum 16 ULP,
  maximum scaled error `1.3878e-17`.

These are separate populations; easy output matches should not dilute gradient
exceptions. The scaled-error gate is not a ten-ULP guarantee. Ordinal gradient
exceptions reach 3,974 (`sw_cratio`), 1,528 (`sw_cratio_cs`), and 5,861 ULP
(`sw_cumulative_cs`); corresponding large exceptions exist in the recorded
corpus metrics.

GEV needs a more explicit follow-up: current maximum ULP distances at points
0/1/2 are **25/77/256**, while recording-time metrics were **4/41/80**.
Fresh CmdStan answers match the committed reference bit for bit at all three
points. This rules out reference drift as the explanation for this discrepancy.
It does not identify which intervening Stanli/compiler/build change caused it;
no matched historical-binary bisection was performed. GEV's maximum scaled
error is `2.8422e-14`. Preserve this evidence and investigate it before accepting
an optimization; do not widen the gate or overwrite historical metrics.

## Profile and next experiment

Opcode instrumentation puts **88.2%** of GEV gradient time inside `OP_LOOP`,
with about two thirds of that in reverse differentiation. Instrumentation
changes costs; the separate native profile gives a second view:

- Of 3,061 sampled stacks, **764 (25.0%)** end in
  `structured_loop_backward` and **570 (18.6%)** in
  `structured_loop_forward`. These self samples include inlined helpers.
  The source rebinds primal/adjoint pointers and dispatches a kernel for each
  frozen call; the profile supports reducing this repeated setup, but does not
  precisely partition every instruction into dispatch versus useful work.
- **645 samples (21.1%, inclusive)** are under `lmultiply_2bwd`, already inside
  the loop totals. Its generic implementation in
  [`scalar_binary.cpp`](../../runtime/kernels/scalar_binary.cpp) opens a nested
  Stan autodiff tape, constructs argument/output buffers and seed operations,
  and differentiates again for each scalar call. Allocation, stack management,
  and seeding are substantial alongside the actual logarithm and pullback.
- The frozen GEV stream contains 523 instructions, 403 kernel calls, 40 branch
  guards, 567 primal cells, and 442 reverse entries. It has **zero scalar
  segments** despite existing segment/register-program support. Call pointer
  and adjoint-pointer pools occupy 12,568 and 9,320 bytes; primal and adjoint
  arrays occupy 4,536 and 3,536 bytes. This is a small scalar execution problem,
  unlike the large recording frames in ctsem.
- The Laplace variants spend 66.9% and 53.7% of instrumented time in forty
  `OP_ISLAND` calls per gradient. They are useful controls for shared scalar
  execution work, but the GEV profile does not prove an identical root cause.

The next bounded experiment should address **scalar backward setup first**:
test a single-output path through the existing Stan var overload, with direct
adjoint seeding, avoiding the generic vector buffers and extra seed graph.
Keep the vector/broadcast path as fallback. This preserves upstream function
implementations and tests whether setup accounts for enough of `lmultiply`'s
cost to matter. It is a hypothesis, not an implemented or verified optimization.

Then test lowering eligible scalar regions into the existing compact program
or segment representation, avoiding per-scalar `KernelCtx` reconstruction in
forward/reverse replay. Eligibility must follow scalar shapes, supported
operations, effects, and dependency provenance. Branch changes must retain
guards and replay fallback; arithmetic and adjoint accumulation order must
remain defined. Do not add a second VM, unroll by model name, or copy large
contexts per iteration to remove pointer setup.

Before keeping either change: compare mixed data/parameter overloads, broadcast
and alias behavior, non-unit seeds, zero/NaN/infinity cases, rejection behavior,
and full model outputs against Stan; preserve measured ULP fidelity. Repeat the
same paired timings with identical-binary controls and all seeds, then measure
preparation, first/warm gradients, full inference, retained/peak memory, and
binary size on ordinary models as well as GEV. Require a demonstrable benefit
without shifting costs onto ordinary preparation or memory. Use the current
thirteen-model baseline; do not substitute the older issue timings.

## Reproduction and validation

The evidence packet contains exact commands and per-run identities. The
primary invocation, repeated with a fresh output path for each frozen filter,
was:

```sh
python3 harnesses/corpus_bench.py \
  .cache/issue374-20260921/cmdstan deps/posteriordb OUT.tsv \
  --filter s2_gev --sampling --cmdstan-runtime-multiple 3 \
  --bench .cache/issue374-20260921/build-release/bench_grad \
  --run .cache/issue374-20260921/build-release/stanli_run \
  --cmdstan-stanc .cache/issue374-20260921/compilers/stanc \
  --vectorize-probe .cache/issue374-20260921/compilers/stanli-vectorize-probe
```

Set the recorded Stan/OpenMP/BLAS thread variables to one. The plan lists the
ten substring filters and asserts that their union is exactly the thirteen
intended models, without duplicates. Profiling follows timing, never overlaps
it. The native profiles use `/usr/bin/sample` for four seconds at 1 ms during
an eight-second warm-gradient window; these are diagnostic processes, not
additional performance samples.

The fresh Release build passed `sampler_parity` and `benchmark_timer` focused
CTest checks. Numerical checks, three-point shipped/MIR equivalence, phase-probe
value checks, and all 32 CLI reproduction checks passed. No production source
changed, and the investigation does not claim a full CI or installed-package
validation run. Issue #374 remains open: GEV/Laplace execution, GEV numerical
drift, pathological sampling, and the CLI memory gap are distinct remaining
questions.
