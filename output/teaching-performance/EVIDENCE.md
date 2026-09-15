# Current teaching sweep: evidence and reproduction

Run `fae5494c296cd547` assessed all **199 fixtures**: 13 educational, 62
Rethinking, and 124 brms. There are 183 complete comparisons, ten capped cases,
and six cases stopped before sampling. The [full appendix](README.md) retains
every outcome and defines the measurement and diagnostic boundaries.

## Build and numerical checks

The benchmark started from clean branch revision `bce6148e`; runtime/compiler
sources match main `2ae6c1d029ec6c29fc9be5cb7a72cdf94eccfd7f`. The fresh Release
build used Apple M3 Ultra, 96 GiB RAM, macOS 26.6.2 ARM64. All recorded executable,
compiler, benchmark-source and frozen-input hashes were checked again after the
sweep. The manifest distinguishes Stanli's patched Math dependency from the
pristine CmdStan oracle dependencies.

- `benchmark-manifest.json`: run, configuration, source, dependency, binary, and input identities.
- `build-identity.json`: additional compiler/shared-runtime hashes and CMake configuration.
- `benchmark-summary.tsv`: the original v3 benchmark summary, including incomplete rows.
- `teaching-results.json` and `teaching-timings.csv`: per-model timings, caps, gradient measurements, diagnostics and collection summaries.
- `sampling-diagnostics.json`: diagnostics computed after timing finished, from the retained four-chain CSVs.
- `numerical-replay-all.txt`: current 315/316 policy replay; `m14.11` times out before comparison. The replay includes existing ill-conditioning exceptions, expected support gaps and domain refusals; it is not 315 strict finite-gradient passes.
- `educational-verification.json`: all 13 educational fixtures pass their three-point numerical/output checks and sampling smoke tests.
- The [Rethinking evidence](../rethinking-report/README.md) contains its separate 61/62 current replay and unchanged references for the failed fixture.
- `r-ecosystem-tests.txt` and `guide-examples.txt`: fresh-runtime acceptance and execution of both guide examples.
- `r-first-posterior.csv` / `.txt`: three separately launched R sessions, median 0.127 s. These were measured after the corpus run and diagnostics finished.
- `native-call-benchmark.txt`: fresh-runtime paired density/gradient adapter timings, measured after the startup experiment.
- `SHA256SUMS`: checksums of this evidence set and the raw archive.

## Raw evidence

`raw-evidence-fae5494c296cd547.tgz` (499 MB) is retained locally and excluded
from Git. The committed manifest, per-model results and checksums identify it;
the archive itself is not hosted by this PR. It contains:

- `run/`: frozen model/data inputs, command events, stdout/stderr, model records,
  per-seed draws, generated model headers, and MIR. Compiled executables are omitted.
- `analysis/`: numerical and sampling logs, diagnostic jobs/results, build identity,
  and the R acceptance/startup measurements.
- `source/`: benchmark, report, diagnostic, and generator sources used for this evidence.
- `provenance/`: fixture inventories, data provenance, licenses and coverage notes.

The original run remains at
`/tmp/stanli-rethinking/latest/teaching-v3.tsv.run`. The older run
`37fa26701db14f56` and its raw archive remain separately retained and are not
mixed into these results. Earlier `be0a0c8d` timings are superseded here.

## Reproduction

Use the recorded toolchain and fresh build. The measured command was:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb NEW.tsv \
  --corpus teaching --bench build-teaching-latest/bench_grad \
  --run build-teaching-latest/stanli_run --sampling --cmdstan-runtime-multiple 3
```

To regenerate diagnostics/tables from the archive, extract it, then follow the
commands in the [appendix](README.md#reproduce-the-tables), using its `run/`
directory. Regenerate diagnostic jobs after relocation because the recorded
job list has absolute paths. Do not run diagnostic analysis, builds or
compression concurrently with timed measurements.

The McElreath PDF is generated separately by `tools/report_rethinking.py` from
the same full run, diagnostics, and the Rethinking numerical replay. Its
narrative is explicitly tied to this audited run.
