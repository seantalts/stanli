# Rethinking report evidence

The [four-page PDF](../pdf/rethinking-report.pdf) and
[editable report](rethinking-report.md) cover all 61 book call sites plus the
separately labeled hurdle fixture. They use full teaching sweep
`fd5e0ecacddc7047`, measured runtime/compiler revision `6e462c2e`. Earlier
measurements are retained separately and are not mixed into these results.

## Results

- **Speedup:** all 61 book call sites completed all four seeds. Median
  CmdStan/Stanli complete CLI speedup: **1.576x**. Each fixture's ratio is in
  the appendix. The report uses speedups; original times remain in the CSV.
- **Numerics:** 32,349 values compared at three fixed parameter vectors for
  each of 62 fixtures. Maximum absolute differences: **1.88e-9 log density**,
  **1.86e-9 gradient component**, **3.55e-15 model output**. The tables separate
  these quantities, and the CSV also records ULP distances.
- **Sampler measurements:** divergence counts, maximum R-hat and minimum
  bulk ESS remain in the CSV. The report has no diagnostic verdict column.

## Files

- `rethinking-timings.csv`: speedups, absolute differences, ULP distances, original timings and sampler measurements for every fixture.
- `rethinking-results.json`: per-seed outcomes, caps, numerical results and run manifest.
- [Shared sampling diagnostics](../teaching-performance/sampling-diagnostics.json): post-run diagnostics for all 199 teaching fixtures; the report selects Rethinking rows.
- `numerical-replay.txt`: the original 62-fixture numerical/output replay.
- `numerical-errors.json`: separate maxima and counts for log density, gradients and model outputs, with checker and reference hashes.
- `numerical-values.json.gz`: every paired reference/Stanli value from the descriptive replay using the same frozen build and inputs.
- `numerical-reference-subset.json.gz`: all 62 fixtures' pinned CmdStan references, unchanged by the performance fixes.
- Shared [run manifest](../teaching-performance/benchmark-manifest.json), [benchmark summary](../teaching-performance/benchmark-summary.tsv), and [build identity](../teaching-performance/build-identity.json): identities and original measurements.
- `SHA256SUMS`: checksums, including the PDF and shared raw archive.

The **550,455,487-byte** raw archive is retained locally at
`../teaching-performance/raw-evidence-fd5e0ecacddc7047.tgz` and excluded from
Git. See the [shared evidence index](../teaching-performance/EVIDENCE.md) for its
checksum, contents, limits and reproduction commands. The original run is
`/tmp/stanli-teaching-perf/teaching-v6.tsv.run`. All four PDF pages were rendered
and visually inspected.

## Measurement boundary

Each timing is one CLI chain with 1,000 warmup iterations and 1,000 retained
draws, summarized across four seeds. Stanli includes model preparation;
CmdStan starts from a compiled model. Compilation is measured separately as
Stan translation plus the ordinary C++ model build, excluding the gradient
benchmark driver. Adding stages gives a first-fit estimate, not a directly
timed four-chain R invocation.

CmdStan runs first for each seed. Stanli's cap is the smaller of 3× that
CmdStan CLI duration and 900 seconds. Both engines use their CSV defaults:
eight significant digits for CmdStan and 17 for Stanli. Numerical verification
uses separate high-precision values. Incomplete and flagged models remain in
the report. Preparation and performance follow-ups are tracked in [#372](https://github.com/seantalts/stanli/issues/372)
and [#373](https://github.com/seantalts/stanli/issues/373).

## Regenerate the presentation

The raw archive and original replay remain unchanged. The additional numerical
files were produced afterward with the same frozen checker and model/data bytes.
From the repository root, with that checker and run directory available:

```sh
python3 tools/report_rethinking_numerics.py RUN_DIRECTORY \
  output/rethinking-report/numerical-reference-subset.json.gz \
  output/teaching-performance/build-identity.json \
  build-teaching-perf/stanli_check output/rethinking-report
python3 tools/report_rethinking.py RUN_DIRECTORY \
  output/teaching-performance/sampling-diagnostics.json \
  output/rethinking-report/numerical-replay.txt output/rethinking-report
```

The numerical exporter verifies the checker hash and frozen input hashes;
it does not change the CmdStan references or benchmark results. The report
builder requires ReportLab. A new build must have its own recorded identity.
