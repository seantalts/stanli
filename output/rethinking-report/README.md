# Rethinking report evidence

The [four-page PDF](../pdf/rethinking-report.pdf) and
[editable report](rethinking-report.md) cover all 61 book call sites plus the
separately labeled hurdle fixture. They use full teaching sweep
`fd5e0ecacddc7047`, measured runtime/compiler revision `6e462c2e`. Earlier
measurements are retained separately and are not mixed into these results.

## Results

- **Numerics:** all **62/62** fixtures pass the three-point replay, including
  outputs: 32,349 compared values, worst scaled discrepancy `1.48e-13` against
  the unchanged `1e-9` gate. The m14.11 preparation timeout is fixed.
- **Sampling:** all **61/61** book calls plus the hurdle fixture complete four
  timed seeds in both engines. Stanli has lower median CLI runtime on all 61
  book comparisons; the median CmdStan/Stanli ratio is **1.576**. m13.6 completes
  every seed. Phylogenetic m14.11 takes 11.21 seconds versus CmdStan's 13.09.
- **Quality:** **49/61** book calls plus the hurdle fixture complete and meet
  the diagnostic screen in both engines. Timing alone does not establish
  reliable inference.

## Files

- `rethinking-timings.csv`: every book call and the supplemental fixture, with timings and diagnostics.
- `rethinking-results.json`: per-seed outcomes, caps, numerical results and run manifest.
- `sampling-diagnostics.json`: post-run diagnostics for all 199 teaching fixtures; the report selects Rethinking rows.
- `numerical-replay.txt`: all 62 Rethinking numerical/output checks.
- `numerical-replay-all.txt`: the full 316-model replay under the repository's existing policies.
- `numerical-reference-subset.json.gz`: all 62 fixtures' pinned CmdStan references, unchanged by the performance fixes.
- `benchmark-manifest.json`, `benchmark-summary.tsv`, `build-identity.json`: identities and original measurements.
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
