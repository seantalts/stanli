# COM-Poisson: sampling performance and numerics

**CmdStan/Stanli is 0.838× for complete sampling**, up from
0.573× in the matched previous build. Higher means faster Stanli;
1× is equal runtime. Stanli's median complete-process time fell
31.6%. It takes about 19% longer than CmdStan on this workload.

Three four-seed confirmation rounds give **0.840×, 0.843×, and 0.834×**.
All runs finish within the unchanged 3× CmdStan cap. These results supplement
the [earlier cap fix](../followup-com-poisson/README.md) and the frozen teaching sweep.

| Seed | Ratio of median runtimes | Range across three trials |
| --- | ---: | ---: |
| 1 | 0.988× | 0.972–1.000× |
| 2 | 0.810× | 0.801–0.820× |
| 3 | 0.815× | 0.801–0.828× |
| 4 | 0.799× | 0.798–0.807× |

The overall ratio exceeds the 0.8× target. Seed 4 remains at its boundary;
its unrounded median ratio is 0.79944×. Individual timings vary, so this is
not a claim that every run is faster than the target or than CmdStan.

## Numerical differences from CmdStan

| Quantity | Three corpus points: max absolute | Max ULP | 28 posterior/boundary points: max absolute | Max ULP |
| --- | ---: | ---: | ---: | ---: |
| Log density | 2.84e-14 | 2 | 1.14e-13 | 4 |
| Gradient | 2.13e-14 | 17 | 2.27e-13 | 127 |
| Constrained/generated outputs | 0 | 0 | 8.88e-16 | 1 |

All **12 before/after Stanli draw CSVs are byte-identical**. Density, gradients,
and outputs at the additional 28 points are bitwise identical before and after.
The extra-point errors are computed before decimal serialization; hexadecimal
floating-point values preserve the exact comparisons in the archived JSON.

## What changed

- Reuse the derivative workspace after proving that every read is initialized
  on every path. Each executor owns its storage; unsupported cases keep fresh
  buffers. The COM-Poisson model admits all 20 replay regions.
- Remove constant initializers overwritten before any read or branch.
- Allocate scalar-math callback buffers from Stan's existing autodiff arena,
  preserving the operations, overloads, and derivative accumulation order.

Stan Math functions and derivatives are unchanged by this optimization; the
allocator is its existing public API. The dependency setup has a pre-existing
[one-line adjoint ODE initialization patch](existing-stan-math-ode.patch), already
on main since September 11; it is recorded in provenance and unrelated to this model.

The model, data, arithmetic, random seeds, sampler settings, stopping rules,
and numerical tolerances are unchanged. Workspace reuse adds approximately
1.65 MB per executor for this model, released with the executor.

## Measurement protocol

Apple M3 Ultra, macOS ARM64, Release build. Three fixed rounds of seeds 1–4,
each with 1,000 warmup iterations and 1,000 retained draws. Each executable is
launched once without arguments before timing. Runs are serial with numerical
thread limits set to one, and no builds, tests, profiling, or diagnostics overlap
the final timing blocks. CmdStan runs first to establish the per-seed cap;
previous/candidate Stanli order alternates. The cap is
min(3 × that CmdStan duration, 900 seconds).

The measured interval includes the entire sampling CLI process and Stanli
source compilation. CmdStan model compilation is excluded. This is warm-executable,
fixed-budget sampling time, not installation time or time to equal effective sample
size. The headline ratio divides the medians of all 12 corresponding runtimes.
The original 199-model report is unchanged.

## Evidence

- [Sampling summary, absolute times, MADs, and CSV hashes](sampling-summary.json);
  [raw draws and command logs](sampling-raw.tar.gz).
- [All allocation/compiler probes and both confirmation sets](sampling-experiments.json.gz),
  with source patches, executable identities, commands, caps, and results.
- [Original-point numerics](original-point-numerics.json) and
  [additional-point numerics](additional-point-numerics.json).
- [Five-pair gradient canaries](gradient-canaries.json.gz) and
  [median/MAD summaries](gradient-canary-summary.json). These measure fixed-point
  gradients, separately from the complete-sampling ratios above; all values match.
- [253 runtime tests](runtime-tests.txt), [316 corpus references](numerical-replay.txt)
  covering 1,008,755 values, and [495 R expectations](r-ecosystem-tests.txt), with
  no failures, warnings, or skipped R tests.
- [Instrumented corpus run](sentinel-replay.txt): every replay register starts
  with a NaN sentinel, and a derivative reaching that sentinel fails the run.
  The same 316 reference checks pass. [Instrumentation](sentinel-instrumentation.patch.gz)
  is absent from the measured production binary.
- [Fable advice](fable-advice.md), [follow-up review](fable-review.md), and
  [review disposition](review-disposition.md).
- [Source patch](source.patch.gz), [provenance](provenance.json),
  [reproduction scripts and point references](reproduce.tar.gz), and [checksums](SHA256SUMS).
