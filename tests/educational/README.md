# Educational Stan models

Thirteen Aalto teaching programs and their supplied JSON fixtures, imported
unchanged from the user-supplied `stan_course_corpus.zip` on 2026-09-14.
`manifest.json`, each `provenance.json`, the R data excerpts, and `licenses/`
retain the archive's source attribution, BSD-3 notices and fixture hashes.
Six fixtures use transcribed lesson data; seven are explicitly synthetic.
The original archive report is `IMPORT_README.md`; its links to unbundled
reports/scripts describe that archive, not this test integration. Its
`not_run` fields are historical import metadata, not current test results.

These are browser-transcribed teaching sources, not commit-pinned upstream
copies. Source/data bytes are checked against the import manifest before
every run. No teaching model is rewritten to make a test pass.

## Correctness on every CTest run

```sh
cmake --build build-rel --target stanli_check stanli_run -j2
ctest --test-dir build-rel -R 'test_educational|test_run_timings' --output-on-failure
```

`tools/check_educational.py` runs the shipped source compiler and compares
log density (proportional, with Jacobian), every unconstrained gradient,
output names/order, constrained and transformed parameters, and generated
quantities against actual CmdStan outputs at three deterministic points.
The numerical gate is `abs(a-b)/max(1, abs(a), abs(b)) <= 1e-9`, matching the
existing external corpus gate and allowing cross-platform libm rounding.
All values must be finite; missing points, columns, models or outputs fail.
The RNG streams for fixed-point generated quantities match the existing
`ref_driver.cpp`/`stanli_check` contract, including rejection-sampling loops.

Each model also runs 100 warmup + 100 saved draws from source, requiring
complete finite CSV output including every generated quantity. This short
run is a sampling smoke test, not a convergence claim. The existing sampler
unit and statistical tests remain necessary. Generalized Pareto's unused
CDF/LCDF definitions are not claimed as executed coverage.

References are recorded independently of Stanli's answers. Regeneration is
explicit, requires CmdStan, and must be reviewed; never regenerate to hide
a discrepancy:

```sh
python3 tools/check_educational.py --record --build build-rel \
  --cmdstan deps/cmdstan --stanc deps/stanc3/stanc
```

The reference archive records toolchain commits, compiler flags, and
source/data hashes. Ordinary CI needs neither CmdStan nor source downloads.

## Live performance gate

After `tools/dev_setup.sh --corpus --no-test` provides the Release build and
CmdStan reference toolchain:

```sh
python3 tools/check_educational.py --benchmark --build build-rel \
  --cmdstan deps/cmdstan --output build-rel/educational-benchmark/results.json
# Or: cmake --build build-rel --target check_educational_performance
```

Every model must achieve **CmdStan wall time / Stanli wall time >= 0.5**;
**Pareto must achieve >= 1.0**.
This is a per-model floor, never an average. Compilation errors, timeouts,
malformed CSV, nonfinite outputs and missing timing evidence fail the run.
Failures remain failures; there are no model exclusions or relaxed floors.

Both engines run one chain, initialize at unconstrained zero, use the same
seed and default NUTS adaptation settings, and save 1000 draws after 1000
warmup iterations. After one untimed filesystem/process warmup pair, three
paired repetitions alternate engine order. Both write all CSV outputs to
disk at 17-digit precision within the timed interval. CmdStan uses `--O1`
vectorization and `-O3`; Stanli must be built in Release mode. Run on an idle
machine. Raw timings, medians, min/max, median absolute deviations, binary
identities, settings, CSVs and stderr are retained beside the report.

The gate compares complete CLI runs against **already compiled CmdStan**:
Stanli source compilation, JSON preparation, adaptation, sampling, generated
quantities and output are all included. CmdStan source compilation is
reported separately and cannot subsidize a slow Stanli run. Stanli's
`--timings` additionally separates preparation, NUTS, and output (including
generated quantities); the enclosing wall timer also includes process
startup/teardown. These phase numbers are diagnostic, not subtracted from
the gate.

Sampled parameter means are also compared using six combined batch-means
Monte Carlo standard errors (20 contiguous batches per chain, with a floor
from between-chain variation). This broad regression check allows distinct
random trajectories; it does not certify convergence or effective sample
size. Full fixed-point generated-output comparisons provide the stronger
deterministic oracle for those computations.

The live test is a separate build target because it compiles 13 CmdStan
executables and timing on busy CI runners is not stable. Run it when changing
compiler, sampler or output performance. Default CTest always runs the
recorded correctness oracle and sampling smoke tests.


## Main benchmark suite and results page

The main [corpus runner](../../harnesses/corpus_bench.py) discovers all 13
fixtures by default alongside posteriordb; `--corpus educational` selects
only this collection. `--stanli-only` refreshes existing rows without adding
new rows with missing CmdStan measurements. The [main results page](../../docs/benchmarks.md#educational-models)
includes the paired end-to-end results, rendered from the retained JSON by
`tools/corpus_table.py --educational REPORT.json`.
