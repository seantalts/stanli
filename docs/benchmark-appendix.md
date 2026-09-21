# Benchmark experiment details

This appendix describes the single full-corpus experiment behind the
[benchmark table](benchmarks.md#full-corpus). Results and the final run identity
are pending. All 319 application models use the same build, configuration and
measurement protocol; failures remain part of the inventory.

## Run identity

The run manifest is authoritative for the Stanli revision and tracked changes,
CmdStan/Stan/Math revisions, compiler flags, executable hashes, machine,
thread settings, source/data hashes and protocol settings. The current sweep
will publish that manifest with its results. No performance claim is assigned
to a different build or substituted from another run.

| Item | Current experiment |
| --- | --- |
| Scope | 319 application models, `--corpus all` |
| Gradient trials | Six alternating engine pairs at one shared point |
| Per-process gradient warmup | At least 200 ms |
| Per-process gradient measurement | At least 250 ms |
| Sampling | Seeds 1–4, one chain per seed and engine |
| Iterations per chain | 1,000 warmup and 1,000 retained draws |
| Stanli build | Release, clang++, `-O3 -DNDEBUG`; full runtime, threads enabled, lite mode disabled |
| Runtime source base | `45ea5cca1e4f583e57963438a0a3994841c5ab69`; final checkpoint and executable hashes will be recorded in the manifest |
| Reference header compiler | Pinned stock `deps/stanc3/stanc`, with no additional stanc flags |
| CmdStan | 2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6` |
| Stan Math | `5252d51d47c1d5e78005fc043ad996fad6dd8da8` |
| Timeouts per command | Gradient 60 s; build 900 s; sampling 900 s; no relative runtime cap |
| Host | macOS arm64, 32 logical CPUs, 96 GiB RAM |
| Run ID | Pending manifest publication |
| Completion | Pending full sweep and post-run diagnostics |

Normal background services remain active on the host. No other benchmark or
heavy build runs concurrently; this is not a dedicated idle machine. The
harness requests one thread per engine for Stan, OpenMP and common BLAS
implementations, while the runtime itself is built with thread support.

## Warm gradients and numerical checks

Both engines evaluate the sampling log density with proportional terms and
Jacobian adjustment at coordinate `i` equal to
`0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Model construction and the first
validation evaluation occur outside the timed gradient window.

Each fresh process warms for at least 200 ms, then evaluates batches for at
least 250 ms. An evaluation that exceeds the window is measured in full.
The drivers report elapsed nanoseconds and evaluation count. The harness
collects six pairs, alternating Stanli/CmdStan and CmdStan/Stanli order, with
only one engine running at a time.

Every accepted pair must match log density and all gradient components within
scaled error `abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Nonfinite values,
unequal widths, failed commands and numerical mismatches produce no speedup.
The broader [reference replay](../TESTING.md#comparison-with-cmdstan-on-complete-models)
independently checks three points and recorded output values. Before timing,
all 329 referenced models passed that replay at three points, comparing
1,020,194 values under its existing numerical policy. The largest observed
scaled error was `9.38e-13`, and the largest ULP deviation was 7,040; this
is not a claim that every comparison falls within a fixed 10-ULP bound.

The reported gradient ratio is the median of the six within-pair
CmdStan/Stanli ratios, with MAD. Per-engine median latencies, dispersion and
all individual observations are retained. Small differences should be read
with their dispersion; a ratio close to one does not establish a reliable win.

## Compilation, preparation and complete sampling

These timings answer different questions:

- **CmdStan compilation:** building the ordinary model executable, separately
  from its timed CLI execution. Compiling the gradient driver is setup work,
  not the user's model-build latency.
- **Preparation:** binding an executor from existing MIR, excluding source
  compilation and model evaluation. This is not source-to-model compilation.
- **Complete CLI run:** process startup, Stanli source compilation and
  preparation, adaptation, sampling, generated quantities, CSV output and
  teardown. CmdStan starts from its already-built executable.

Each engine runs seeds 1–4 with 1,000 warmup iterations and 1,000 saved draws
per seed, default NUTS settings and random initialization. Engine order
alternates by seed. Each gradient command has a 60-second timeout; model
builds and sampling commands each have a 900-second timeout. There is no
reference-relative runtime cap. These settings are frozen in the manifest
before timing begins. CSV formatting uses each CLI's recorded
precision, so output cost is part of its measured user path.

A CLI median requires all four seeds to complete with the expected draw count,
consistent column widths and finite output. A failed or capped seed prevents
an aggregate for that engine. No average of surviving seeds replaces it,
and no dataset or iteration budget is reduced mid-run. Compilation plus CLI
time is a sum of measured stages, not a directly timed cold-cache first fit.

## Sampling diagnostics and interpretation

Diagnostics are computed after timing, across each engine's four saved chains.
The parameter columns come from the independently generated CmdStan header;
fixed structural entries are checked before omission from diagnostics.

A clear diagnostic screen requires no retained-draw divergences or tree-depth
hits, finite R-hat at most 1.01 and bulk effective sample size at least 400 for
every nonconstant parameter. Reports also retain tail ESS and minimum E-BFMI.
Missing chains or undefined required diagnostics cannot pass the screen.

A completed run is not necessarily a well-mixed posterior. Different numerical
trajectories can produce different leapfrog counts and adaptation. CLI time
therefore measures a fixed iteration budget, not time to equal inference
quality. The table keeps diagnostic warnings beside timings rather than
excluding difficult models from the results.

## Artifacts and reproduction

The runner writes `build/corpus-current/benchmark-summary.tsv` and its sibling
`benchmark-summary.tsv.run/` directory. Published summaries and diagnostics
will be retained in `output/corpus-performance/`. The artifact set will contain:

- The immutable run manifest and retained Stan/data inputs.
- Per-model results, raw gradient pairs, exact commands, event logs, exit
  statuses and timeout reasons.
- The summary TSV with every model's recorded outcome.
- Sampling diagnostics and the generated full-corpus report.

The commands below fix this experiment's configuration. The default reference
header compiler is `deps/stanc3/stanc`, with an empty `--stancflags` value;
Stanli uses its shipped vectorization probe and source compiler. The manifest
retains their exact versions and hashes. Run diagnostics only after the timed
sweep finishes.

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/benchmark-summary.tsv \
  --bench build/bench_grad --run build/stanli_run \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --sampling --seeds 1 2 3 4 --iter-warmup 1000 --iter-sampling 1000 \
  --gradient-timeout 60 --build-timeout 900 --sample-timeout 900
python3 tools/corpus_diagnostic_jobs.py \
  build/corpus-current/benchmark-summary.tsv.run build/corpus-current/diagnostic-jobs.json
Rscript tools/summarize_corpus_bench.R \
  build/corpus-current/diagnostic-jobs.json build/corpus-current/sampling-diagnostics.json
python3 tools/report_corpus.py \
  build/corpus-current/benchmark-summary.tsv.run \
  build/corpus-current/sampling-diagnostics.json output/corpus-performance
```

Resume only with the same inputs, binaries and frozen settings. The
[protocol specification](benchmark-protocol.md) describes the manifest checks,
process timing and failure policy in detail.
