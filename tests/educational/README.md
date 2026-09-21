# Models from Aalto Stan lessons

Thirteen Aalto teaching programs and their supplied JSON fixtures, imported
unchanged from the user-supplied `stan_course_corpus.zip` on 2026-09-14.
`manifest.json`, each `provenance.json`, the R data excerpts, and `licenses/`
retain the archive's source attribution, BSD-3 notices and fixture hashes.
Six fixtures use transcribed lesson data; seven are explicitly synthetic.
The original archive report is `IMPORT_README.md`; its links to unbundled
reports/scripts describe that archive. Its `not_run` fields are historical
import metadata, not current test results.

These are browser-transcribed sources, not commit-pinned upstream copies.
The shared [corpus inventory](../../tools/corpus_inventory.py) validates their
source and data hashes against the import manifest. The files stay here to
preserve attribution; they use the same numerical and benchmark paths as
other application models.

## Numerical and sampling tests

The common reference archive, [`docs/corpus-refs.json.gz`](../../docs/corpus-refs.json.gz),
contains all 13 models at three deterministic points. The
[corpus replay](../../tools/verify_refs.py) compares log density (proportional,
with Jacobian), every unconstrained gradient, output names/order, constrained
and transformed parameters, and generated quantities with recorded CmdStan
answers. The scaled-error gate is `abs(a-b)/max(1, abs(a), abs(b)) <= 1e-9`.
Missing points, names, outputs and nonfinite values fail. The migrated records
retain compiler/toolchain identity and source/data hashes.

```sh
cmake --build build-rel --target stanli_check stanli_run -j2
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 4
ctest --test-dir build-rel -R 'test_corpus_sampling|test_run_timings' --output-on-failure
```

All 13 fixtures opt into the generic sampling-smoke check through inventory
metadata. Each runs 100 warmup iterations and 100 saved draws from source,
requiring complete finite CSV output with exactly the reference output names
and order. Run the same check directly with:

```sh
python3 tools/check_corpus_sampling.py --build build-rel
```

This short run is a sampling smoke test, not a convergence claim. Sampler
unit and statistical tests remain necessary. Generalized Pareto's unused
CDF/LCDF definitions are not claimed as executed coverage.

References come from CmdStan, independently of Stanli's answers. Regeneration
uses the [common recorder](../../tools/verify_sample.py) and is an explicit,
reviewed operation; never regenerate to hide a discrepancy. To explicitly
refresh the complete existing reference set:

```sh
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb --from-refs --jobs 4
```

Ordinary replay needs no CmdStan build or source download for these local fixtures.

## Benchmarks and retained experiments

The [common benchmark runner](../../harnesses/corpus_bench.py) includes these
models by default. `--corpus educational` is an optional provenance filter.
New runs use the same [measurement protocol](../../docs/benchmark-protocol.md)
and reporting path as the rest of the application corpus:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/corpus-v4.tsv
python3 tools/publish_corpus_bench.py /tmp/corpus-v4.tsv /tmp/corpus-v4-report
```

The [main results page](../../docs/benchmarks.md) is organized by measurement.
The [September 14 archive](../../notes/performance/benchmark-history.md) and
[detailed investigation](RESULTS.md) preserve the original collection-specific
experiments, raw timings, posterior-mean comparisons and per-model performance
floors. Those historical thresholds do not define a separate active test tier.
