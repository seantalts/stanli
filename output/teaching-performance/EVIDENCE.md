# Teaching sweep: evidence and reproduction

Run `fd5e0ecacddc7047` covers all **199 fixtures**: 13 educational, 62
Rethinking, and 124 brms. There are **193 complete comparisons, one capped
cases, and five cases stopped before sampling**. Stanli has the lower median
CLI time on 187 completed comparisons; six remain slower. The practical
CmdStan/Stanli ≥ 0.8 timing target is met by 189 fixtures, with four below
and six without a complete comparison. The [appendix](README.md)
retains every outcome and diagnostic flag.

## Build and checks

The measured Release runtime/compiler revision is `6e462c2e`, on Apple M3 Ultra,
96 GiB RAM, macOS ARM64. It includes the performance fixes in this PR. Executable,
compiler, configuration, benchmark-source and frozen-input hashes were rechecked
after the sweep. The manifest distinguishes Stanli's patched Math dependency
from the pristine CmdStan oracle dependencies.

- `benchmark-manifest.json`, `build-identity.json`, and `post-run-identity-check.txt`: exact identities and post-run checks.
- `benchmark-summary.tsv`: original benchmark summary, including incomplete rows.
- `teaching-results.json`, `teaching-timings.csv`, and `sampling-diagnostics.json`: timings, caps, gradients and diagnostics for every fixture. Diagnostics ran after timed measurements ended.
- `baseline-comparison.json`: comparison with the separately retained original sweep; model/data bytes and sampler configuration match. These complete-run before/after timings were not interleaved.
- `numerical-replay-all.txt`: **316/316** under the existing reference policies, including documented ill-conditioning exceptions, support gaps and domain refusals. This is not 316 strict finite-gradient passes.
- `educational-verification.json` / `.txt`: **13/13** educational numerical/output checks and sampling smoke tests.
- [Rethinking evidence](../rethinking-report/README.md): **62/62** three-point numerical/output checks, 32,349 values, largest scaled discrepancy `1.48e-13`.
- `runtime-tests.txt`: **249/249** runtime tests pass.
- `r-ecosystem-tests.txt` and `guide-examples.txt`: runtime acceptance without skips, warnings or failures, and both generated-model guide examples.
- `ci-validation.json`: [full platform CI](https://github.com/seantalts/stanli/actions/runs/35060736527) passes at `6e462c2e`, the measured revision. Includes Linux/macOS/Windows R acceptance, full compiler comparisons and sanitizers.
- `sanitizer-checks.txt`: retained raw-log checksums for the same-source ASAN (249 tests) and TSAN (three tests) run.
- `r-first-posterior.csv` / `.txt`: three local fresh R sessions, median **0.120 s**, measured after the sweep and diagnostics. `r-first-posterior-ci-{mac,linux,windows}.csv` retains the independent CI platform measurements.
- `native-call-benchmark.csv` / `.txt`: five alternating pairs of 20,000 density/gradient calls through the direct and native-stanfit APIs.
- `SHA256SUMS`: checksums for this evidence set and the current raw archive.

Published text logs normalize line endings and trailing whitespace; exact
original logs are retained in the raw archive.

## Raw evidence

`raw-evidence-fd5e0ecacddc7047.tgz` (**550,455,487 bytes**) is retained locally
and excluded from Git. Its SHA256 is
`30840d8b8ed7ae4734ec3d69f41fff54bad51c91d9f4cf2227e00667182125ad`.
The archive is not hosted by this PR. It contains:

- `run/`: frozen inputs, command events, stdout/stderr, model records, per-seed CSVs, generated headers and MIR. Executables are omitted.
- `analysis/`: numerical/sampling logs, diagnostics, identities, CI summaries and R measurements.
- `source/`: benchmark, report, diagnostic and generator sources used for this evidence.
- `provenance/`: inventories, data provenance, licenses and coverage notes.
- `research/`: separately labeled intermediate optimization experiments and profiles; these are not substituted into the final sweep.

The original run remains at `/tmp/stanli-teaching-perf/teaching-v6.tsv.run`.
The pre-optimization run `fae5494c296cd547` and its 499 MB archive remain
separately retained. The interrupted partial m12.6 attempt is retained under `interrupted/` in the
archive and excluded from aggregation; the resumed sweep retained all 37
already complete model records. The earlier v5 run and its archive also remain
separately retained.

## Later fixes remain separate

The [GP and inverse-Gaussian follow-up](followup-4db5dca2/README.md) records
four-seed checks on the runtime changes integrated in `4db5dca2`. It includes
source equivalence, numerical comparisons and diagnostics. These checks do
not replace frozen full-sweep rows. COM-Poisson and the remaining performance
gaps are still open; this report is a measured checkpoint, not completion of
all support work.

## Reproduction

Use the recorded toolchain and a fresh Release build:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb NEW.tsv \
  --corpus teaching --bench build-teaching-perf/bench_grad \
  --run build-teaching-perf/stanli_run --sampling --cmdstan-runtime-multiple 3
```

To regenerate tables, extract the archive and follow the commands in the
[appendix](README.md#reproduce-the-tables). Regenerate diagnostic jobs after
relocation because their CSV paths are absolute. Do not run analysis, builds
or compression concurrently with timed measurements. The McElreath PDF uses
the same run and diagnostics plus the separate Rethinking numerical replay;
its narrative is explicitly tied to this audited checkpoint.
