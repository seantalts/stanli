# Corpus benchmark protocol, version 3

The benchmark measures **warm gradient latency**. Full inference is a separate,
explicit phase. A numerical comparison must pass before a timing pair is accepted.
The shared [inventory](../tools/corpus_inventory.py) supplies application
models from every source collection; language-conformance fixtures remain
numerical tests. Collection selectors are optional provenance filters and do
not change the measurement contract. Retained [historical runs](benchmark-history.md)
keep their original revisions and protocols; they are not pooled into a new sweep.

## Fixed measurement contract

Both engines use the same model, data, unconstrained parameter vector, sampling
log density (`propto=true`) and Jacobian adjustment. At coordinate `i`, the fixed
point is `0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Every accepted pair compares
log density and **every gradient coordinate** using the existing scaled-error
gate `abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Non-finite results, unequal
widths and failed commands produce no speed result.

The two C++ drivers share `tools/benchmark_timer.hpp`:

1. Construct and bind the model outside the timed window; evaluate once to
   reject invalid points before measurement.
2. Warm for at least **200 ms**. Small batches grow during warmup to amortize
   clock reads. There is no limit based on parameter count or 1,000 evaluations.
3. Measure batches for at least **250 ms**, recording the actual elapsed
   nanoseconds and evaluation count. An evaluation that exceeds the window is
   measured in full; reported time is never clipped to the requested window.
4. Start a fresh process for each measurement. Collect **six paired rounds**,
   alternating stanli/CmdStan and CmdStan/stanli order. Both binaries are built
   before the paired phase. One engine runs at a time.

These defaults are measurement settings, not a threshold for declaring a
performance win. They can be changed before starting a run, and then become
part of its immutable identity. Gradient process timeouts, build timeouts and
sampling timeouts are separate settings.

Report each engine's median nanoseconds per gradient and median absolute
deviation (MAD). Also report the median and MAD of the **within-pair speedup
ratios**. Preserve every raw measurement. A marginal difference needs an A/A
control and a predetermined confirmation experiment; it is not established by
one favorable row.

Preparation from existing MIR is measured separately, excluding stanc and
model evaluation. It is not labeled source-to-model compilation. A gradient
driver's C++ build time is a setup event, not a user's CmdStan model-build time.

The runner uses the shipped vectorized compilation pipeline via
`deps/stanc3/stanli-vectorize-probe` for gradients and the CLI's default compiler for sampling.
`--cmdstan-stanc` (alias `--stanc`) and `--stancflags` select the reference
header compiler; the generated header is shared by its gradient driver and
sampler build. All compiler choices and executable hashes are in the manifest.
Earlier reports retain their recorded compiler commands and hashes; their
measurements must not be relabeled as results from the current build.

## Reproducible runs

```sh
./tools/dev_setup.sh --no-build       # builds the compiler and its probe
cmake --build build-rel --target bench_grad stanli_run -j 4
# New output path; gradients only, all application models:
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/corpus-v3.tsv
# Optional source filter (also available: posteriordb, brms, educational):
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/rethinking-v3.tsv --corpus rethinking
```

The standard summary is rendered with:

```sh
python3 tools/corpus_table.py /tmp/corpus-v3.tsv
```

Complete sampling reports and diagnostic screens use
[`tools/report_corpus.py`](../tools/report_corpus.py). Its source-collection
column records provenance; every selected model follows the same acceptance
and reporting rules. Run diagnostics after the timed sweep finishes:

```sh
python3 tools/corpus_diagnostic_jobs.py /tmp/corpus-v3.tsv.run /tmp/corpus-jobs.json
Rscript tools/summarize_corpus_bench.R /tmp/corpus-jobs.json /tmp/corpus-diagnostics.json
python3 tools/report_corpus.py /tmp/corpus-v3.tsv.run /tmp/corpus-diagnostics.json output/corpus-performance
```

These commands require a completed run with `--sampling`; they do not rerun
benchmarks. The former collection-specific performance floors belong to the
historical experiments that introduced them.

A sibling `OUT.tsv.run/` contains:

- `manifest.json`: protocol settings, machine, thread settings, hashes of
  inputs, executables and driver sources, and CmdStan/Stan/Math checkout
  identities, including tracked diffs, linked TBB/Sundials build products and
  local make configuration;
- `inputs/`: retained model and data bytes;
- `events.jsonl` and `logs/`: exact command lines, order, exit codes, phase
  durations, process timeouts, load averages, stdout and stderr;
- per-model `.result.json`: raw paired samples, numerical comparisons,
  preparation measurements and optional sampling results.

`OUT.tsv` is a derived summary of that run. `--resume` accepts only the same
inputs, binaries and protocol settings. An existing unversioned TSV is rejected.
There is no candidate-only refresh that combines a fresh measurement with an
old comparator. Historical version-1 data stays separate until a reviewed
version-3 run replaces it.

Version 3 waits directly for process exit and enforces deadlines with a
separate timer. Version 2 used timeout polling, which could add up to 50 ms
of observer delay to short runs. Its wall times are retained as historical
evidence and must not be mixed with new measurements.

The command runner distinguishes failure from timeout, keeps logs on both,
and kills the process group on timeout on POSIX hosts. Failed measurements
cannot be interpreted as successful command output. The runner requests one
thread for Stan, OpenMP and the common BLAS implementations for both engines.

## Optional inference measurements

Add `--sampling` to run 1,000 warmup iterations and 1,000 retained draws for each
of seeds 1, 2, 3 and 4. Engine order alternates by seed. The seed list must have
an even number of distinct seeds; the list, iteration counts and timeout are
frozen in the manifest. The default sampling timeout is 900 seconds per command.

CSV validation checks the retained draw count, column widths and finite values
before accepting a successful exit. These are observed CLI wall times, including
CSV output at each CLI's default precision (CmdStan 2.39 uses eight significant
digits; stanli uses 17). Output formatting costs are therefore part of each
engine's measured user path. The independent numerical oracle uses high-precision
values, not these sampling CSVs. These timings do not measure
ESS per second or establish mixing quality. CmdStan's ordinary model binary is
built separately with `make`; its model-build duration is recorded separately.
If any seed fails or times out, that engine has no aggregate sampling time:
there is no average over the seeds that happened to finish. Timeouts are
explicitly censored observations with their actual limits and logs retained.
No dataset is shortened and no sampling budget is changed mid-run.

### Optional cap relative to CmdStan

`--sampling --cmdstan-runtime-multiple 3` runs CmdStan first for each seed,
then limits stanli to the smaller of three times that matching CmdStan CLI
runtime and the absolute `--sample-timeout`. C++ compilation is excluded from
the reference runtime. If CmdStan fails, produces invalid draws or times out,
the corresponding stanli run is recorded as not run because no valid reference
duration exists. A stanli timeout is retained as a censored result, never as
a completed run at the cap.

This sampling policy uses a fixed reference-first order instead of alternating
order; that limitation must accompany its results. Gradient trials still
alternate. The multiplier is frozen in the manifest, and changing to this
policy requires a fresh run. Earlier runs remain separate.

## Validation before a corpus sweep

- Deterministic-clock tests cover tiny work, slow work, batching and overshoot.
- Runner tests inject nonzero exits, timeouts and invalid driver output.
- Manifest tests reject changed inputs, binaries, settings and legacy appends.
- Focused real-model measurements cover a scalar, a hierarchical model and
  an ordered regression with many observations, plus an identical-binary A/A
  control. No full sampling sweep is needed to test the measurement protocol.

### Local validation

On Apple M3 Ultra (macOS ARM64), a fresh Release build passed the timer and
preparation checks and all 17 recorder tests. Six paired rounds each passed
for `ch09_mp`, `eight_schools_noncentered` and `ch12_m12_5`; the worst scaled
density/gradient error was `1.83e-14`.

Identical-binary controls returned paired median ratios of 1.002 (scalar stanli)
and 1.004 (ordered-model CmdStan), with ratio MADs of 0.039 and 0.013. These
controls demonstrate why small differences require further measurement.
A separate scalar sampling smoke test completed two seeds per engine, with
50 warmup and 50 retained draws per seed. An unchanged resume reused its records;
a changed measurement window was rejected. These checks validate the harness,
not a new corpus performance claim.
