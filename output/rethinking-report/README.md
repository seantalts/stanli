# Rethinking report evidence

- `../pdf/rethinking-report.pdf`: the four-page report prepared for Richard McElreath.
- `rethinking-report.md`: editable report and full timing table.
- `rethinking-timings.csv`: all 61 book call sites and the separate hurdle fixture; full-precision timings and diagnostic summaries.
- `rethinking-results.json`: per-seed outcomes, limits, summary values and the run manifest.
- `sampling-diagnostics.json`: R-hat, effective sample sizes, divergences and tree-depth counts from posterior 1.7.0.
- `numerical-replay.txt` and `numerical-reference-subset.json.gz`: fresh replay results and the pinned CmdStan references for these 62 fixtures.
- `benchmark-manifest.json` and `benchmark-summary.tsv`: the original run identity and summary.
- `raw-evidence.tgz`: locally retained model/data inputs, all stdout/stderr logs, per-seed CSVs, model records, analysis jobs and fixture provenance/licensing (about 337 MB). This archive is excluded from Git; its checksum is recorded below alongside the committed artifacts.
- `SHA256SUMS`: checksums of the report and supporting artifacts.

## Measurement boundary

Each timing is a single CLI chain with 1,000 warmup iterations and 1,000 retained draws. Four seeds per engine were measured. CmdStan ran first for each seed; stanli was capped at the smaller of 3x that CmdStan CLI time and 900 seconds. There are no medians of surviving seeds for incomplete sets. CmdStan compilation is the sum of the matching Stan-to-C++ event and C++ model-build event; it excludes the separate gradient-driver build. First-fit estimates add these recorded compilation stages to the median CmdStan run time.

The measured run is `37fa26701db14f56`. Earlier uncapped and exploratory runs are excluded. Raw commands and diagnostic job paths retain the original machine's paths; they document execution and are not relocatable commands. The original run directory is `/tmp/stanli-rethinking/mcelreath-report/timings-cap3.tsv.run`.

The runtime was built from base revision `be0a0c8d`, with the benchmark changes
identified by the manifest's source hashes. These measurements describe that
build; subsequent changes on `main` have not been remeasured in this report.

## Reproduction

Use the versions and source hashes in the manifest and the corpus provenance. From the repository, the measured command was:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb NEW.tsv \
  --corpus rethinking --bench build-benchmark-v2/bench_grad \
  --run build-benchmark-v2/stanli_run --sampling --cmdstan-runtime-multiple 3
```

This is a substantial full-corpus sampling experiment, not a quick smoke test. To rebuild the report from the retained results, use `tools/report_rethinking.py` with the run directory, diagnostics JSON, numerical replay log and output directory. Its narrative is explicitly tied to this audited run. `tools/summarize_rethinking_bench.R` recomputes diagnostics from the retained job list; update absolute input paths if the archive has been relocated.
