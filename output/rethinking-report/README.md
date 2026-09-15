# Rethinking report evidence

The [four-page PDF](../pdf/rethinking-report.pdf) and
[editable report](rethinking-report.md) cover all 61 book call sites plus the
separately labeled hurdle fixture. These results come from the fresh full
teaching sweep `fae5494c296cd547`; runtime/compiler sources match main
`2ae6c1d0`. Earlier `be0a0c8d` numbers are superseded, not mixed into this report.

## Results

- **Numerics:** 61/62 fixtures pass the current three-point replay, including
  outputs: 32,316 compared values, worst scaled discrepancy `6.50e-14` against
  a `1e-9` gate. `m14.11` times out during preparation before comparison;
  [#372](https://github.com/seantalts/stanli/issues/372) also records Linux CI reproduction.
- **Sampling:** 58/61 book call sites, plus the hurdle fixture, complete all
  four timed seeds in both engines. `m12.6` hits the absolute 900-second cap on
  seed 1; `m13.6` hits its 3× cap on seed 3. No surviving-seed aggregate is used.
- **Quality:** 47/61 book call sites, plus the hurdle fixture, complete and
  meet the diagnostic screen in both engines. Timing alone is not evidence
  that a fit supports reliable inference.

## Files

- `rethinking-timings.csv`: every book call and the supplemental fixture, with full-precision timings and diagnostics.
- `rethinking-results.json`: per-seed outcomes, caps, numerical results and the full run manifest.
- `sampling-diagnostics.json`: post-run diagnostics for the complete 199-fixture teaching sweep; the report selects the Rethinking rows.
- `numerical-replay.txt`: the successful replay of the 61 fixtures other than `m14.11`.
- `numerical-replay-all.txt`: the full current-build replay, retaining the `m14.11` failure.
- `numerical-reference-subset.json.gz`: all 62 fixtures' pinned CmdStan references; the failed model's references are preserved.
- `benchmark-manifest.json`, `benchmark-summary.tsv`, `build-identity.json`: identities and original measurements for the full teaching sweep.
- `SHA256SUMS`: checksums, including the PDF and shared raw archive.

The 499 MB raw archive is retained locally at
`../teaching-performance/raw-evidence-fae5494c296cd547.tgz` and excluded from
Git. It contains all inputs, command logs, per-seed CSVs, headers, analysis
sources, and provenance. See the [shared evidence index](../teaching-performance/EVIDENCE.md)
for its layout, limits, and reproduction commands. The original run directory
is `/tmp/stanli-rethinking/latest/teaching-v3.tsv.run`.

## Measurement boundary

Each timing is one CLI chain with 1,000 warmup iterations and 1,000 retained
draws, summarized across four seeds. Stanli includes model preparation;
CmdStan starts from a compiled model. Compilation is measured separately as
Stan translation plus the ordinary C++ model build, excluding the gradient
benchmark driver. Adding those measured stages gives a first-fit estimate,
not a directly timed four-chain R invocation.

CmdStan runs first for each seed. Stanli's cap is the smaller of 3× that
CmdStan CLI duration and 900 seconds. The absolute cap on `m12.6` does not
establish a greater-than-3× slowdown. The report preserves incomplete and
flagged models. Both engines use their CSV defaults, eight significant digits
for CmdStan and 17 for Stanli; numerical verification uses separate
high-precision reference values.
