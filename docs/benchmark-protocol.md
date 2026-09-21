# Corpus benchmark protocol, version 4

The benchmark measures setup and warm gradient latency, then calculates a
**setup + 20,000-gradient time estimate**. It does not run full sampling.
The shared [inventory](../tools/corpus_inventory.py) supplies application
models from every source collection; language-conformance fixtures remain
numerical tests. Collection selectors only filter provenance.

## Gradients and numerical acceptance

Both engines use the same source, data, unconstrained parameter vector,
sampling log density (`propto=true`) and Jacobian adjustment. Coordinate `i`
is `0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Every accepted pair compares log
density and every gradient coordinate using scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Non-finite values, unequal widths
and failed commands produce no accepted pair. This is a numerical acceptance
gate, not a fixed ULP bound or proof of complete sampler conformance.

The drivers share [benchmark_timer.hpp](../tools/benchmark_timer.hpp):

1. Construct/bind the model and validate the point outside the timed window.
2. Warm for at least 200 ms; grow small batches to amortize clock reads.
3. Measure batches for at least 250 ms, retaining elapsed nanoseconds and
   evaluation count. A slow evaluation is measured in full, never clipped.
4. Use a fresh process for each measurement and collect six pairs, alternating
   Stanli/CmdStan and CmdStan/Stanli order. Only one engine runs at a time.

Report each engine's median latency and median absolute deviation (MAD), and
the median/MAD of the within-pair CmdStan/Stanli ratios. Ratios above one favor
Stanli. A small difference must be read with its dispersion; confirming a
marginal improvement requires a separate predetermined experiment.

## Setup and the 20,000-gradient estimate

The fixed budget of **20,000 gradient evaluations** is recorded in the manifest.
With gradient latencies in seconds:

```text
Stanli estimate  = source-to-MIR + preparation + 20,000 × median gradient time
CmdStan estimate = stanc translation + C++ build + 20,000 × median gradient time
```

Stanli source-to-MIR time is the elapsed compiler-probe process using the
shipped vectorization pipeline, including process startup. Preparation is the
median of six fresh-process measurements from existing MIR and JSON to a bound
executor, excluding gradient evaluation. This boundary differs from in-process
Python/R model construction.

CmdStan translation uses the selected stanc compiler and flags. Its ordinary
model executable is built separately from the gradient driver. Common CmdStan
dependencies and precompiled headers are prepared in advance; per-model
executables are fresh. CmdStan data/model initialization is not added to the estimate. Gradient-driver
compilation is retained as setup evidence but is not charged to the estimate.

Each compilation phase is measured once per model, with normal filesystem
caches. The estimate sums measured stages; it is not a timed cold-cache first
fit. Missing or invalid components produce no estimate. Accepted gradients
remain visible if a later ordinary-model build fails.

The fixed workload corresponds to an assumption of 2,000 iterations at ten
gradient evaluations each. It excludes adaptation, tree building, generated
quantities and output, and does not predict a particular model's NUTS runtime.
Numerical and sampler correctness remain covered independently by
[TESTING.md](../TESTING.md).

## Running the experiment

```sh
./tools/dev_setup.sh --no-build
cmake --build build-rel --target bench_grad -j 4
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/corpus-v4.tsv --bench build-rel/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900
```

`--corpus` selects a source collection; `--filter` selects matching model IDs.
`--stanc` (alias `--cmdstan-stanc`) and `--stancflags` select the reference
compiler. Stanli uses `deps/stanc3/stanli-vectorize-probe` by default.
A distinct output path is required for each changed configuration.

The sibling `OUT.tsv.run/` retains:

- An immutable manifest: source/input/executable hashes, protocol, machine,
  threads, compiler choices, upstream checkouts, library hashes and build flags.
- Frozen Stan/data inputs, exact commands, load averages, elapsed times, exit
  status, timeout reasons and stdout/stderr logs.
- Per-model raw paired observations, numerical deviations, preparation
  observations, compilation events and the derived summary row.

`OUT.tsv` is derived from these records. `--resume` requires identical inputs,
binaries, sources and settings. Failures stay in the inventory. Timeouts kill
the command's process group and never become a completed observation at the
limit. Gradient commands default to 60 seconds and builds to 900 seconds.
The runner requests one thread for Stan, OpenMP and common BLAS libraries.
Run timed sweeps serially, and record whether the host is shared or idle.

Publish complete runs, then regenerate public pages:

```sh
python3 tools/publish_corpus_bench.py /tmp/corpus-v4.tsv output/corpus-performance
python3 tools/gen_docs.py
```

Documentation generation also requires the optimized-reference artifacts
specified in the [appendix](benchmark-appendix.md). The current table validates the entire
inventory, raw pairs and recomputed estimates before publishing.
It never fills missing cells from historical measurements.

## Earlier protocols and validation

Version 3 optionally measured full sampling across four seeds. Its retained
reports and diagnostic tools remain interpretable as historical evidence.
Version 4 removes that phase and its sampling flags. Version 3 introduced
direct process-exit timing; version 2's timeout polling could add observer
delay to short commands. Measurements from these protocols are not pooled.

Focused tests cover timer batching/overshoot, numerical gates, invalid output,
nonzero exits/timeouts, immutable manifests, estimate arithmetic, missing
components and report provenance. Existing model replay and sampler smoke
checks remain separate from performance measurements.
