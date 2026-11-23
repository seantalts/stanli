# Issue #657: first mixed-model diagnostic

The reported 500-subject slowdown remains **unreproduced and unresolved**.
The original generated program/data, observation count, versions, hardware,
sampler settings and ESS are missing. A focused public search did not recover
them. The diagnostic below supplies reproducible scaffolding, not the
reporter's model or a demonstrated fix. No runtime code changed.

## Baseline and evaluator

- Issue: [#657](https://github.com/seantalts/stanli/issues/657).
- Fetched default base and source R bindings:
  `0eb880b9b9579cd28ca64c3afa4257cba9adca1e`; clean issue worktree verified
  that `origin/HEAD` was an ancestor before work. The lead owns fetching.
- Official dev runtime supplied by the lead:
  `abi6-0eb880b9b957-Darwin-arm64-threads`, archive SHA256
  `8d955fe96c6e7b2df499ada2f81a0f49c845c4b560042ce2068adba2a7c927c4`.
- CmdStan 2.40.0 at pinned `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`,
  stock pinned stanc with `--O1`; reference driver built with repository
  `compile_cmd(..., opt="-O3", sundials=False)`, FP contraction disabled.
  Shared dependency sources/builds were read only. Model artifacts were
  built in the issue worktree's `.cache/issue657/probe`.
- Host: Apple M3 Ultra, macOS arm64; R 4.6.1, brms 2.23.0. The globally
  installed Stanli 0.19.1 was not used: current source R package 0.30.1 was
  installed privately into `.cache/issue657/r-lib`.
- [Harness](../../harnesses/issue657_mixed_model.R): Gaussian brms formula
  `y ~ x + (1 + x | subject)`, proper priors, 20 observations per subject,
  deterministic data seeds, J = 10, 50 and 500. The same generated program
  is used for all sizes. The original report's likelihood and group sizes
  are unknown, so this is explicitly a **surrogate**.

The first discriminating checks are whether gradients become expensive with
J and whether additional sampler work could explain longer inference.
Correctness is checked at the origin and a deterministic small nonzero
unconstrained vector. Model construction uses `allow_interpreter=FALSE` so a
fallback cannot silently explain the diagnostic. A passing surrogate does
not resolve the original report.

## Observations

All three models prepared without interpreter fallback and produced finite
density/gradient vectors. Source-to-MIR compilation was 21.4 ms; model
preparation was 15.5, 8.2 and 67.7 ms, respectively. First R gradient calls
were 4.52, 0.226 and 0.103 ms. These are one-process observations with a
finer wall clock; first-call overhead and process order prevent interpreting
them as stable phase comparisons.

**The two timing columns below use different interfaces.** Stanli includes
R `.Call`, allocation and result packaging; CmdStan uses the standalone C++
gradient driver. They are neither matched kernel timings nor a valid
speedup ratio, and do not prove the reported end-to-end issue absent.
They only establish the scale of this bounded diagnostic.

| Subjects / observations | Parameters | Stanli R-call gradient median, µs | CmdStan C++ gradient, µs |
| --- | ---: | ---: | ---: |
| 10 / 200 | 26 | 4.45 | 6.91 |
| 50 / 1,000 | 106 | 10.85 | 31.68 |
| 500 / 10,000 | 1,006 | 66.59 | 311.15 |

Stanli has five batches of 100 gradients; CmdStan has one elapsed-time window
per point (20 ms warmup, 40 ms measurement). Measurements were serialized
after both agents' reference compilations. There are no counterbalanced
paired runs, posterior evaluation points or complete inference comparisons.
No performance conclusion or optimization is justified from these numbers.

Numerical results over two points per size:

| Subjects | Components within 10 ULP | Worst ULP | Maximum absolute gradient difference |
| --- | ---: | ---: | ---: |
| 10 | 54 / 54 | 7 | 1.07e-14 |
| 50 | 213 / 214 | 61 | 2.27e-13 |
| 500 | 1,981 / 2,014 | 448 | 1.82e-11 |

The worst 500-subject discrepancy is point 1, combined lp/gradient component
890 (gradient index 889, zero based): `0.007297423520031196` versus
`0.0072974235200315846`, absolute `3.89e-16`, relative `5.32e-14`.
This small-magnitude gradient is consistent with cancellation, but the
arithmetic was not profiled to prove its cause. The result does **not**
establish a universal 10-ULP bound, and no tolerance was widened.

A Stanli-only diagnostic sampled one chain for 20 warmup + 20 saved draws,
seed 657, zero initialization and maximum depth 5, with warmup saved.
All output rows were finite. Total leapfrogs were 1,128 / 1,099 / 1,098;
post-warmup depth-limit hits were 19 / 20 / 20. There were 3 / 5 / 5
divergences including warmup (none post-warmup). This short run is unfinished
inferential validation, not evidence of convergence or ESS. No matched
CmdStan sampling run was made, so sampler-work explanations remain open.
Whole R-process peak RSS was 164,855,808 bytes; it includes package/runtime
loading and every size, not isolated per-model peak or retained memory.

## Reproduction and next step

```sh
mkdir -p .cache/issue657/r-lib
R CMD INSTALL --clean --no-multiarch --library=.cache/issue657/r-lib r
Rscript harnesses/issue657_mixed_model.R emit .cache/issue657/probe
R_LIBS=.cache/issue657/r-lib STANLI_RUNTIME=/path/to/libstanli.dylib \
  ISSUE657_SAMPLE=1 Rscript harnesses/issue657_mixed_model.R probe .cache/issue657/probe
```

The independent reference is `tools/bench_cmdstan_grad.cpp`, compiled by
`tools/cmdstan_ref.py:compile_cmd` against the generated header. Its exact
[compile argv](data/issue657-2026-11-23/cmdstan-build-command.json) and
[build durations](data/issue657-2026-11-23/cmdstan-build.json) are retained.
Run each emitted `jJ.json` with `--timed --warmup-ms 20 --measure-ms 40
--points-file jJ.points.json`. The source/data hashes and raw pointwise
values are in the [manifest](data/issue657-2026-11-23/manifest.json),
[Stanli output](data/issue657-2026-11-23/stanli.json) and CmdStan
[J10](data/issue657-2026-11-23/cmdstan-j10.json),
[J50](data/issue657-2026-11-23/cmdstan-j50.json),
[J500](data/issue657-2026-11-23/cmdstan-j500.json) outputs. Generated source
and data remain reproducible local cache artifacts, not executable files
under documentation directories.

The source R wrapper and reference compiled without diagnostics; complete
build logs are retained beside these artifacts. The harness successfully
emitted all three datasets and ran current-runtime compilation, preparation,
gradients and short sampling. `git diff --check` passes. No production change
was made, so no compiler-pipeline corpus comparison was run.

Next: obtain the original program, data and settings; then compare current
Stanli/CmdStan gradients at posterior points and matched full fits, including
leapfrog counts, diagnostics, ESS/s and memory. If recovery is delayed, the
surrogate can be used for a separately labeled matched-interface and
counterbalanced diagnostic. Do not close #657 on this evidence.
