# Teaching sweep: evidence and reproduction

Run `fd5e0ecacddc7047` covers **199 fixtures**: 13 educational, 62 Rethinking,
and 124 brms. It has **193 complete comparisons, one capped case, and five
cases stopped before sampling**. Stanli has the lower median CLI time on 187
completed comparisons. CmdStan/Stanli ≥ 0.8 for 189 fixtures; four fall below
that target and six lack a complete comparison. The [full appendix](README.md)
retains every outcome and sampler diagnostic.

## Published evidence

The measured Release runtime/compiler revision is `6e462c2e`, on Apple M3
Ultra, 96 GiB RAM, macOS ARM64. Executable, compiler, configuration, benchmark
source and frozen-input hashes were rechecked after the sweep. The manifest
distinguishes Stanli's patched Math dependency from the pristine CmdStan oracle.

- [Run manifest](benchmark-manifest.json) and [build identity](build-identity.json): toolchain, executable and input identities.
- [Benchmark summary](benchmark-summary.tsv): original measurements, including incomplete rows.
- [Results](teaching-results.json), [timing CSV](teaching-timings.csv), and [sampling diagnostics](sampling-diagnostics.json): every fixture's timings, caps, gradients and diagnostics. Diagnostics ran after timing ended.
- [Baseline comparison](baseline-comparison.json): unchanged model/data bytes and sampler settings; these complete-run before/after timings were not interleaved.
- [Educational verification](educational-verification.json): all 13 fixtures' numerical/output checks and sampling smoke tests.
- [Rethinking evidence](../rethinking-report/README.md): all 62 fixtures' three-point numerical comparisons, with every paired value retained.
- [Fresh-R timings](r-first-posterior.csv): three local sessions, median 0.120 s; independent [macOS](r-first-posterior-ci-mac.csv), [Linux](r-first-posterior-ci-linux.csv), and [Windows](r-first-posterior-ci-windows.csv) CI measurements.
- [Native-call benchmark](native-call-benchmark.csv): five alternating pairs of 20,000 density/gradient calls through the direct and native-stanfit APIs.
- [SHA256SUMS](SHA256SUMS): checksums for the published evidence and locally retained raw archive.

Validation at the measured revision passed 249 runtime tests, 316 corpus
reference checks under the existing policies, and R ecosystem/guide acceptance
without skips, warnings or failures. The 316-model gate includes documented
ill-conditioning exceptions, support gaps and domain refusals; it is not 316
strict finite-gradient passes. [Full platform CI](https://github.com/seantalts/stanli/actions/runs/35060736527)
includes Linux/macOS/Windows R acceptance, compiler comparisons and sanitizers.
Generated test logs and CI responses are retained outside the source tree.

## Raw evidence and later results

`raw-evidence-fd5e0ecacddc7047.tgz` (**550,455,487 bytes**) is retained locally,
excluded from Git and not hosted with this report. Its SHA256 is
`30840d8b8ed7ae4734ec3d69f41fff54bad51c91d9f4cf2227e00667182125ad`.
It contains frozen inputs, command logs, per-seed CSVs, generated headers/MIR,
analysis, identities, report sources, provenance and separately labeled
research experiments. Executables are omitted. The interrupted partial attempt
is retained separately and excluded from aggregation.

The original run remains at `/tmp/stanli-teaching-perf/teaching-v6.tsv.run`.
Earlier sweeps and their raw archives remain separately retained. Later GP,
inverse-Gaussian, COM-Poisson and residual-model measurements are summarized in
the [brms performance report](../../docs/brms-performance.md). They do not
replace this frozen sweep's rows or change its counts.

## Reproduction

Use the recorded toolchain and a fresh Release build:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb NEW.tsv \
  --corpus teaching --bench build-teaching-perf/bench_grad \
  --run build-teaching-perf/stanli_run --sampling --cmdstan-runtime-multiple 3
```

To regenerate tables, extract the archive and follow the commands in the
[appendix](README.md#reproduce-the-tables). Regenerate diagnostic jobs after
relocation because their CSV paths are absolute. Run analysis, builds and
compression separately from timed measurements. The Rethinking PDF uses this
same run and diagnostics plus its separate numerical replay.
