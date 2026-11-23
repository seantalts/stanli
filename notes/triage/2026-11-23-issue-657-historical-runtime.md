# Issue #657: historical-runtime diagnostic

This second-round check asks whether the release available on the report's
date is much slower on synthetic mixed models than current main. It does
not require the reporter's data or model, and it does not assume their
installed version. It did not expose a large historical gradient collapse
in the four cases tested. Small timing shifts remain uncertain.

## Identity and method

The report was posted on November 17, 2026. GitHub records v0.27.0 published
that day at 14:21 UTC, before the 18:45 UTC report. v0.28.0 was published
November 18. This makes 0.27.0 a useful hypothesis, not a verified version
of the reporter's installation.

The official arm64 0.27.0 archive has SHA256
`9371b54f623ac0a31287144519bc8cc57662e4b0c9fe2a3ae089f01763869fc5`.
Its runtime identifies as `abi6-c32930c15132-Darwin-arm64-threads`;
the tag points to `c491b5cab2999039409c7e60351f655a477384a3`. These two
commits have the same Git tree
`f54c91ebfc0b02eb90e538e231381aaf08802198`. Current main is
`0eb880b9b9579cd28ca64c3afa4257cba9adca1e`, using the previously verified
official development runtime.

Each arm compiles the same Stan source afresh using its own embedded
compiler, prepares the same data, and times native C-ABI gradient loops with
the unchanged `tools/benchmark_timer.hpp` protocol. Source compilation and
preparation are excluded from gradient windows. The host is the same Apple
M3 Ultra. Both other agents paused execution during the measurement window.

There are seven alternating old/current pairs per case; the first pair is
retained but discarded from summaries. Each process uses two deterministic
parameter points, 20 ms warmup and 40 ms measurement windows. No pairs or
points failed: 56 processes and 112 finite density/gradient observations.

The first attempt to link the newer diagnostic against the historical
library failed because 0.27.0 does not export `stanli_grad_err`. Both arms
were rebuilt using the older `stanli_grad` entry point, with otherwise
identical driver code. Both successful builds produced no compiler
diagnostics; the initial integration failure is retained separately.

## Results and limits

The table gives the median paired ratio of old/current gradient latency at
each of the two points. A value above one means the historical arm took
longer. These are historical runtime comparisons, not Stanli/CmdStan ratios.

| Synthetic case | Point 0 | Point 1 |
| --- | ---: | ---: |
| Gaussian correlated noncentered, 500 subjects × 20 observations | 1.064 | 1.089 |
| Gaussian correlated noncentered, 10 subjects × 20 observations | 1.011 | 1.063 |
| Bernoulli correlated noncentered, 500 subjects × 5 observations | 1.002 | 1.036 |
| Gaussian uncorrelated centered, 500 subjects × 5 observations | 1.131 | 1.184 |

Individual small-control ratios span 0.901–1.134. The centered case has a
consistent direction in these six pairs, but there is no A/A noise study or
causal pass ablation. Do not use the small differences as a confirmed
regression or attribution to a particular fix. The measurements do not
reproduce the original 1.57x **Stanli/CmdStan sampling** ratio: they concern
a different comparator, metric and synthetic workload.

No historical full fits, ESS comparisons, memory measurements or independent
historical numerical oracle were run here. Finite output is not a numerical
parity proof. The current-main matched CmdStan and full-fit exploration is
recorded separately in the [mixed-model investigation](2026-11-23-issue-657-investigation.md).

## Retained evidence and reproduction

- [Raw process measurements, losslessly compressed](data/issue657-historical-027-2026-11-23/runs.json.gz): exact run arguments, source/data hashes, finite flags and full gradient vectors, stderr and exit codes, including the discarded pairs.
- [Summary](data/issue657-historical-027-2026-11-23/summary.json): individual paired ratios and native times.
- [Provenance](data/issue657-historical-027-2026-11-23/provenance.json): runtime/archive/tree/driver/binary identities, compile arguments and the legacy driver derivation.
- Build logs: [initial missing-symbol failure](data/issue657-historical-027-2026-11-23/build.log), [historical corrected build](data/issue657-historical-027-2026-11-23/build-legacy.log), [current corrected build](data/issue657-historical-027-2026-11-23/build-current.log).

The synthetic models/data are emitted by `harnesses/issue657_mixed_model.R`.
For equivalent future measurements, compile the native diagnostic
`harnesses/issue657_native_grad.cpp` with `STANLI_BENCH_LEGACY_GRAD` defined
against each runtime, then run identical source/data with `--points 2
--warmup-ms 20 --measure-ms 40`, alternating process order. The exact
transient source hash and derivation used for the retained experiment are
recorded in provenance. Repeating a run creates new evidence; do not
overwrite the recorded samples.
