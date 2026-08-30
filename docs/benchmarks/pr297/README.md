# PR #297: main versus runtime research checkpoint

Measured 2026-08-30 on the rebased runtime, **before any regression fixes**.

- Baseline: `origin/main` at `917c634170420071f76fb6fb90a5c47fd708c7ab`.
- Candidate: `8d8b37f79dd9b5c5e408a33321664fd86a5d4f36`.
- PR: <https://github.com/seantalts/stanli/pull/297>.
- The subsequent benchmark/report commit does not change the runtime.

## Conclusion

The PR is not yet performance-neutral. All 120 posteriordb models were
attempted, and 119 produced timings at a jointly finite deterministic point.
Ten regressions above 5% survived a second, longer run. The median model is
essentially unchanged (paired after/before ratio **1.00149**), but that median
hides significant regressions in the scalar-program interpreter's consumers.

Every LP/gradient output was bitwise identical between builds in all 357
completed posteriordb point comparisons (344,988 values, including identical
nonfinite values at three points). `sir` rejected all three points with the
same exception on both builds. There are **354 fully finite** comparisons.
`dogs_log` is timed at point 2, not its nonfinite point 0. Each build uses the
same point; failing/nonfinite cases are not treated as successful timings.

The extra structural canaries are more mixed: scalar CFG execution improves,
but two newly selected scans regress. The large structured CFG and state-space
canaries exceed a strict 2-ULP internal comparison budget; see below. Passing
the existing CTest suite therefore does not establish that stricter gate.

## Full results and raw samples

| Experiment | Coverage | Per-model paired process samples | Results |
| --- | --- | ---: | --- |
| Production-default corpus | All 120 models, one dataset per model | 5 | [all-model table](corpus/report.md), [samples CSV](corpus/samples.csv), [evidence JSON](corpus/results.json) |
| Longer confirmation | Ten flagged models and two controls | 7 | [table](confirmation/report.md), [samples CSV](confirmation/samples.csv), [evidence JSON](confirmation/results.json) |
| Extra canaries/support | Four canaries, four completed ctsem boundary configurations | 3 for timed canaries | [table](extra/report.md), [samples CSV](extra/samples.csv), [evidence JSON](extra/results.json) |
| Diagnostic feature disables | Ten configurations; same disable on both builds | 5 | [table](diagnostic/report.md), [samples CSV](diagnostic/samples.csv), [evidence JSON](diagnostic/results.json) |

Ratios are **after / before**, so greater than one is slower. The ratio is
the median of *paired* ratios, not the ratio of the two marginal medians.
The JSON retains every sample, quartiles, min/max, numerical comparisons,
input hashes, binary hashes, environment overrides, and separate preparation
profiles. CSV files retain every measured pair; cases without a timing remain
in the JSON/table, not as a fabricated CSV measurement.

### Confirmed corpus slowdowns

| Model | Before gradient (µs) | After gradient (µs) | Paired ratio | Slowdown |
| --- | ---: | ---: | ---: | ---: |
| `accel_gp` | 5.783 | 6.336 | 1.092 | 9.2% |
| `accel_splines` | 6.411 | 6.876 | 1.073 | 7.3% |
| `garch11` | 7.308 | 8.324 | 1.130 | 13.0% |
| `hmm_drive_0` | 107.532 | 128.435 | 1.185 | 18.5% |
| `hmm_drive_1` | 112.995 | 129.634 | 1.149 | 14.9% |
| `hmm_example` | 16.625 | 19.474 | 1.167 | 16.7% |
| `hmm_gaussian` | 171.313 | 195.230 | 1.143 | 14.3% |
| `iohmm_reg` | 164.094 | 188.845 | 1.151 | 15.1% |
| `lotka_volterra` | 21.304 | 26.144 | 1.220 | 22.0% |
| `soil_incubation` | 28.141 | 36.231 | 1.286 | 28.6% |

Controls: `kronecker_gp` remains about 3.8% faster, and `normal_mixture_k`
about 3.7% slower. No corpus model exceeded a 5% speedup in the first pass.
The 5% threshold is a **triage threshold**, not a statistical confidence bound
or a newly accepted regression budget. All paired samples/dispersion remain
available for review.

### Extra canaries and ctsem support

These are separate from the corpus aggregate and used three paired samples.

| Canary | Before gradient (µs) | After gradient (µs) | Paired ratio | Max ULP |
| --- | ---: | ---: | ---: | ---: |
| Scalar CFG (`pr236_island`) | 0.259 | 0.199 | 0.770 | 0 |
| Large structured CFG | 297.257 | 140.267 | 0.472 | 173 |
| Prepared-solve scan | 60671.333 | 71292.167 | 1.175 | 0 |
| State-space scan | 6.503 | 10.424 | 1.603 | 8 |

The prepared-solve graph falls from 2,309 to 8 operations and from 62,087 to
6,897 stored slot elements. The state-space graph falls from 282 to 13
operations. These are genuine representation improvements, **not** evidence
of a gradient-throughput win.

The structured CFG's maximum absolute difference is `2.66e-15`, and the
state-space canary's is `1.07e-14`. These are small roundoff differences, not
evidence by themselves of materially incorrect inference. Nevertheless, they
exceed the strict 2-ULP gate. In particular `test_cfg_native_large` explicitly
uses `1e-11 + 1e-11 * scale`, explaining why it passes. The stricter gate must
be satisfied or any alternative policy explicitly justified; this benchmark
does not silently relax it.

On the retained ctsem MIR, main fails compilation with an unknown
runtime-control integer. The candidate succeeds at all three points for
N=32 and N=33, both default and experimental mixed-carry configurations.
There is no valid main/PR timing ratio. Main also fails the first N=4000
check; the remaining full-size checks were stopped once the absent baseline
made a timing comparison impossible. This is **not** a fresh full-size ctsem
or CmdStan parity result. The partial extra run's metadata intentionally has
no completion timestamp; only its eight completed cases are exported.

## Pattern and proposed fix sequence

Feature-disable tests support two distinct mechanisms. Each diagnostic ratio
still compares **main versus PR**, with the named switch applied to **both**
builds. It does not compare a production runtime against a slower fallback on
the other side. All 30 diagnostic numerical comparisons match bitwise.

| Workload | Default PR/main ratio | With feature disabled on both sides | Switch |
| --- | ---: | ---: | --- |
| `garch11` | 1.130 | 0.936 | `STANLI_NO_ISLAND=1` |
| `accel_splines` | 1.073 | 0.986 | `STANLI_NO_ISLAND=1` |
| `hmm_example` | 1.167 | 1.012 | `STANLI_NO_ISLAND=1` |
| `lotka_volterra` | 1.220 | 1.038 | `STANLI_NO_ODE_DIRECT_RK=1` |
| `soil_incubation` | 1.286 | 1.042 | `STANLI_NO_ODE_DIRECT_RK=1` |
| Prepared-solve canary | 1.175 | 1.008 | `STANLI_NO_SCAN=1` |
| State-space canary | 1.603 | 1.019 | `STANLI_NO_SCAN=1` |

Disabling only native adjoints leaves ratios of 1.051, 1.026, and 1.118 for
GARCH, splines, and the HMM respectively. Thus reverse tracing is not the only
plausible shared cost: the forward/var interpreter and CALL setup must also
be checked. These switches isolate paths; they are **not recommended global
fixes**, since the fallbacks can be slower in absolute time. No experiment
here isolates metadata width from branch/setup overhead individually.

1. **Ordinary interpreter overhead.** The confirmed corpus regressions keep
   the same graph operation/slot counts and numerical values. The PR adds
   trace/replay hooks and reverse trace/pair checks to common interpreter
   loops. `AdjInstr` grows from 40 to 44 bytes. Release disassembly confirms
   both the larger stride and additional dispatch/setup work. Simple direct
   RK right-hand sides repeatedly call `run_adjoint` to construct Jacobians,
   amplifying that shared overhead. This is a mechanism hypothesis, not yet
   an attribution of every percentage point to an individual change.
2. **Separate execution modes once, outside the instruction loop.** Keep a
   plain straight-line specialization with no trace, pair, or replay checks;
   keep traced/replay specializations for programs that actually require
   them. Selection must depend only on immutable program metadata. Check the
   resulting assembly, not just source-level conditions.
3. **Avoid charging ordinary programs for optional metadata/setup.** If the
   plain specialization leaves a regression, independently test side tables
   for trace metadata and reentrant, reusable CALL contexts. Do not restore
   the old single TLS context blindly: nested CALL correctness is part of the
   PR's contract.
4. **Treat new scan selection as a separate issue.** Profile replay, copying,
   retained solves, and reverse accumulation, then fix the cost or retain a
   conservative fallback. A generic profitability test must account for work
   per row and reverse/replay cost, not just graph size. Do not tune eligibility
   against model or variable names.
5. **Numerical gate before default enablement.** Isolate the changed reverse
   accumulation/order behind the canary ULP differences. Preserve the oracle's
   arithmetic order or leave the affected capability opt-in pending an
   explicit numerical-policy decision.
6. **Re-run the same evaluator.** First the minimal interpreter/scan fixtures,
   then the ten flagged models and canaries, then all 120 models with the same
   shared driver, inputs, points and paired sampling. Keep the current results
   as the pre-fix baseline. No runtime fix is included in this report.

## Method and reproduction

- Apple M3 Ultra, 32 physical cores, 96 GiB RAM; macOS 26.6.2 arm64.
- Apple clang 21.0.0.21000101; matched clean Release Ninja builds, `-O3
  -DNDEBUG -ffp-contract=off`, macOS deployment target 11.0.
- Single-thread environment: `OMP_NUM_THREADS`, `OPENBLAS_NUM_THREADS`,
  `VECLIB_MAXIMUM_THREADS`, `MKL_NUM_THREADS` all `1`. All inherited `STANLI_*`
  variables are removed; diagnostic/experimental overrides are explicit.
- posteriordb `28f8d3d6e975315f42aa274a8399f21e07a43b30`; choose the first
  dataset for each model in sorted posterior-metadata order. This is **120
  models**, not every model/dataset posterior pair.
- Stan Math `8f326d14599d3030c626c46532d8e8534c1cdbec`; Stan
  `c96d04115d35cb04f42e45c5a69a82f9704798f1`. Pinned stanc hash in JSON.
- One O1 MIR per model, consumed unchanged by both builds; prepared-solve
  fixture uses O0. Extra ctsem cases use the retained shared MIR.
- LP and every gradient checked at deterministic points 0, 1, 2. Timings use
  the first jointly finite point. The confirmation run checks point 0; the
  full three-point corpus evidence is retained separately.
- Fresh process per preparation and gradient sample; alternating AB/BA order.
  Each gradient process warms up for at most 1,000 evaluations, checking a
  200 ms cap after each evaluation. Identical evaluation counts on each side
  target 0.2 s/sample for the corpus and 0.5 s/sample for confirmation, with a
  minimum of three evaluations. Large evaluations can exceed those targets.
- Preparation covers file read, data/MIR parsing, compilation, executor
  creation, and bind. It excludes stanc generation. It is not a cold-disk
  benchmark. Peak RSS is the gradient **whole-process** maximum, including
  preparation and warmup, measured by `/usr/bin/time -l`.
- Forward-only time is separately measured after the gradient loop. Subtracting
  it from gradient time is only a reverse-cost estimate, not an instrumented
  reverse sweep. Detailed preparation profiles run outside all timing samples.
- No builds, tests, or other benchmark runs were intentionally overlapped with
  timing. OS scheduling/turbo/background-system noise is not eliminated;
  alternating pairs and repeated samples mitigate but do not remove it.

Build both revisions with matching dependencies and flags:

```sh
cmake -S "$BASE_SOURCE" -B "$BASE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSTANLI_STANC_EXECUTABLE="$STANC" \
  -DSTANLI_FAST_TEST_TUS=ON
cmake -S "$PR_SOURCE" -B "$PR_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSTANLI_STANC_EXECUTABLE="$STANC" \
  -DSTANLI_FAST_TEST_TUS=ON
cmake --build "$BASE_BUILD" --target bench_grad stanli_check -j8
cmake --build "$PR_BUILD" --target bench_grad stanli_check -j8
python3 harnesses/runtime_ab.py \
  --before "$BASE_BUILD" --after "$PR_BUILD" --build-drivers \
  --before-ref 917c634170420071f76fb6fb90a5c47fd708c7ab \
  --after-ref 8d8b37f79dd9b5c5e408a33321664fd86a5d4f36 \
  --stanc "$STANC" --pdb "$POSTERIORDB" \
  --output "$NEW_OUTPUT" --samples 5 --seconds .2
python3 harnesses/runtime_ab_export.py "$NEW_OUTPUT/results.json" "$NEW_EXPORT"
```

`--build-drivers` links this checkout's common point-aware `bench_grad.cpp`
against each revision's headers and runtime libraries using its existing
Ninja compile flags. This matters: original main's driver has only point 0,
which is nonfinite for `dogs_log`. The new option changes parameter setup
outside the timed loops; it is not a runtime optimization. For non-Ninja
builds, manually link an equivalent shared driver and pass `--benchmark NAME`.

To reproduce the four canaries, replace `--pdb` with
`--cases docs/benchmarks/pr297/canaries.json` and use `--samples 3 --seconds .2`.
The state-space data are retained alongside that manifest. Additional case
manifests can set `before_env` and `after_env` objects; apply the identical
disable to both objects for diagnostic comparisons. Confirmation uses the
same corpus command with repeated `--model NAME`, `--samples 7 --seconds .5
--points 0`, and a fresh output directory.

The original raw stdout/stderr, copied MIR/data, commands, and exact measurement
source snapshots are retained locally under `/private/tmp/stanli-pr297-corpus`
and `/private/tmp/stanli-pr297-confirmation`; the extra and diagnostic runs use
the corresponding `-extra` and `-diagnostic` suffixes. The committed portable
exports intentionally omit the external model inputs and per-value outputs.

The older `docs/benchmarks.md` corpus table predates this iteration. Earlier
ctsem research reports provide targeted/historical results, not this PR's
full-corpus A/B. None of those CmdStan timings are mixed into this experiment.

## Validation

- Clean Release runtime build: **121/121** configured CTest entries passed
  before the benchmark tooling addition.
- After adding the harness test entry: **122/122** passed with
  `STANC=/path/to/pinned/stanc ctest --test-dir "$PR_BUILD" --output-on-failure -j8`.
  A first invocation without `STANC` failed the 41 lit cases because this
  worktree does not contain `deps/stanc3/stanc`; explicitly selecting the
  pinned compiler resolved all 41. No runtime fix was made for that setup error.
- Benchmark parser/accounting/export tests pass; point-selection CLI checks
  cover all three valid points and missing, duplicate, out-of-range, negative,
  and preparation-mode point options. Shared-driver rebuild smoke checks
  `dogs_log` at the jointly finite point 2.
- `tools/format.sh --check` passes with clang-format 22.1.8.
