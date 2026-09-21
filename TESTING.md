# Why should I trust this?

stanli is a reimplementation of the Stan runtime. This page lists what is
tested, what each test can show, where it runs, and the known limits. Each
claim links to the code or artifact behind it.

The evidence comes from two directions. First, stanli is compared with
CmdStan: both runtimes evaluate the same model and data at the same fixed
points in unconstrained parameter space, comparing the log density, every
gradient component and, where recorded, every value a draw writes. These are
test inputs; agreement at three points does not establish agreement
everywhere. A generated suite adds 24,277 function
signatures and language constructs that applied models rarely use. Second,
stanli's own execution paths are compared with each other. The runtime can
evaluate parts of a model on an operation graph, a MIR interpreter or a
register machine, and it applies several graph passes; those configurations
must agree bitwise, which catches errors a single path would hide. Sampler
configuration is tested separately, because a pointwise gradient cannot see
it.

## Numerical policy

This is the one place the acceptance rules live.

- The project goal is agreement with CmdStan within 10 ULP most of the time,
  with measured and explained exceptions. A ULP is one step between adjacent
  doubles at a given magnitude. Passing a scaled-error gate is not proof of a
  10 ULP bound.
- The default is 2 ULP. Bitwise agreement is reported but is not a gate.
- Reassociation-class kernel changes (reductions, matvec, gather backward,
  softmax backward) may reach 10 ULP when the commit message states that
  budget and the baseline moves in the same commit.
- A density merged across loop lanes, where stanli sums in one call what
  CmdStan sums once per iteration, may reach 30 ULP. The models using that
  budget are listed in [`tools/corpus.py`](tools/corpus.py) with the measured
  value and its cause.
- A larger distance is acceptable when a high-precision reference shows
  stanli at least as close to the true value as CmdStan, with the reference
  recorded beside the model. A kernel that replaces a nested-tape backward
  with a closed form is held to such a reference:
  [`tools/matrix_pullback_hp_check.py`](tools/matrix_pullback_hp_check.py)
  evaluates the `matrix_exp` and matrix-solve adjoints at 60 decimal digits,
  and the error relative to each matrix's largest entry must not worsen.
- Unit tests and cross-path tests use bitwise equality. A kernel change that
  widens one records the new limit at the assertion.
- Corpus replay uses a 1e-9 gate on scaled error
  `|a-b| / max(|a|, |b|, 1)`, against 1e-10 when references are recorded;
  the wider gate covers Apple's math library against other platforms'.
  Three `kronecker_gp` points and every point of the three brms
  Gaussian-process models (`sw_gp`, `i320_gp_expquad`, `s2_gp_by_gr`,
  smallest Cholesky pivots 1.1e-12, 3.7e-12 and 1.8e-12) use limits derived
  from their measured deviations. Points CmdStan refuses require matching
  rejection. Anything beyond these budgets is a bug.

Kernels keep the reference summation order where practical, and paths with
an allowance are documented at the call site: the elementwise `log` kernel
uses Eigen's packet implementation, which can differ from the system
library by 1 ULP, while the packet `exp` path is not used because it moves
`kronecker_gp` outside its gate
([`runtime/kernels/eltwise_expr.cpp`](runtime/kernels/eltwise_expr.cpp),
line 333). [`tests/test_matvec.cpp`](tests/test_matvec.cpp) shows the
contract at kernel scale: reordering a matrix-vector sum stays inside the
1e-9 corpus gate and still fails the test by 1 ULP.

## Overview of the checks

The shared [corpus inventory](tools/corpus_inventory.py) supplies numerical
replay, sampling smoke tests and benchmark discovery. Performance follows
the [benchmark method](docs/benchmarks.md#how-we-measure) and is not a
correctness gate.

| check | question | acceptance rule | schedule |
| --- | --- | --- | --- |
| unit tests | Does one operation or pass agree with stan-math? | Bitwise; recorded limits of at most 2 ULP (10 for reassociation) | source-changing PRs |
| compiler producer parity | Do native, js_of_ocaml and Windows compilers emit identical compact-v2 bytes? | Byte-for-byte identity on fixture models; JS API, error, warning and rollback checks | native/JS on PRs; Windows after merge |
| MIR wire cost | Is compact v2 cheaper than legacy MIR? | Eight Schools decode time and bytes each at most half of legacy | after merge, nightly |
| corpus comparison | Do the 329 recorded models match CmdStan at three fixed inputs? | 1e-9 scaled error; documented limits for `kronecker_gp` and the brms GP models; rejection parity; `KNOWN_GAPS` models must keep failing | source-changing PRs |
| corpus sampling smoke | Do selected models produce complete saved draws? | 100 draws after 100 warmup iterations, exact output names, finite values | source-changing PRs, in CTest |
| cross-path matrix | Do stanli's execution paths agree? | Bitwise, except ledger entries | source-changing PRs, in CTest |
| transformation A/B | Do the graph passes preserve results? | Passes on and off agree within 1e-11 | manually after pass changes |
| MIR vectorization A/B | What changes when the compiler's loop-vectorization pass is off? | Both modes pass the reference checks; a confirmed gradient-time regression fails | after merge, nightly |
| installed client checks | Do the built packages work through Python, BridgeStan and R? | APIs, sampling, outputs and ecosystem consumers pass with no missing-runtime skips | source-changing PRs |
| BridgeStan C-ABI comparison | Does the C interface agree with reference BridgeStan? | Four fixture models pass value, name, count and shape checks | after merge, nightly |
| downstream `stanr` embedding | Can the R package vendor this checkout? | `stanr` builds and its Stanli-backend tests pass | every release |
| generated conformance sweep | Do generated cases agree, and which are unsupported? | 10 ULP by default; reviewed per-case policy | nightly |
| coverage baseline | Did a verified generated case stop verifying? | No loss; obsolete policy exceptions removed | in the nightly sweep |
| integrated function models | Do ten mother-style models match CmdStan, outputs included? | Name coverage gate; density, gradients and every output within 1e-9 | source-changing PRs, in CTest |
| model census | Do stanc3's 1,231 integration models still lower? | No decrease in per-model classification | manually |
| sampler trace | Is NUTS configured like CmdStan's? | Diagnostic summaries within limits set for large errors | manually after sampler changes |
| AddressSanitizer | Does CTest run clean under ASan? | No diagnostics | after merge and nightly |
| WebAssembly replay | Does the browser build reproduce the corpus? | Same gates; 118 of 119 compiling posteriordb models fit in wasm32 | manually |
| browser compiler on Safari 17 | Do the bundles keep `static` on its class element's line? | No line ends in `static` | source-changing PRs |
| documentation and formatting | Do stamped numbers match their artifacts? | Exact generated-file and formatter checks | every PR |

## Comparison with CmdStan on complete models

[`tools/verify_refs.py`](tools/verify_refs.py) replays every referenced
model in the inventory against CmdStan's recorded log density and full
unconstrained gradient. This is the broadest whole-model comparison in the
repository and has found errors kernel tests did not reach.

[`docs/corpus-refs.json.gz`](docs/corpus-refs.json.gz) holds 329 models at 3
deterministic points each, 987 points and 358,925 values: 119 posteriordb
models that evaluate, 11 language fixtures adapted from stanc3's compiler
tests, 124 models generated by brms 2.23.0, 62 Rethinking fixtures and 13
Aalto lesson fixtures. Values preserve the doubles CmdStan's driver
([`tools/ref_driver.cpp`](tools/ref_driver.cpp)) emitted. 327 of the models
also carry at least one complete `write_array` row at the same points, 978
rows and 661,269 values over constrained parameters, transformed parameters
and generated quantities, with column names compared exactly; both drivers
seed Stan's RNG at 1234, chain 0, so generated-quantity draws compare too.
An absent row means that output is not covered. Per-model coverage is in
[`docs/corpus-status.md`](docs/corpus-status.md).

The references were recorded by
[`tools/verify_sample.py`](tools/verify_sample.py) with CmdStan 2.40.0 at
`d3d5df6a`, Stan `a6806ef8`, Math 5.4.0 at `5252d51d`, stanc3 2.40.0 at
`d58446e6` and posteriordb `28f8d3d6`, on Darwin arm64, built with
`-ffp-contract=off`; stanli sets the same flag project-wide
([`CMakeLists.txt`](CMakeLists.txt), line 113). Both sides use the sampling
log density (`propto=true`) with the Jacobian. Every point is recorded,
including points CmdStan refuses.

Of the 987 points, 939 are `VERIFIED`, 39 imported points are `RECORDED`,
six are `REJECTED_BOTH` and three are `MISMATCH`. The mismatches belong to
`kronecker_gp`, where two eigenvector gradients depend on a covariance whose
smallest eigenvalue gap is 6.5e-17. A model in `KNOWN_GAPS` is one stanli
refuses today; its references are recorded like any other and the entry
suppresses only the failure. When the gap closes the replay reports
`GAP_CLOSED` and stays red until the entry is deleted.

[`tools/wasm_check.sh`](tools/wasm_check.sh) drives the same replay through
the WebAssembly build; 118 of the 119 compiling posteriordb models pass, and
`nn_rbm1bJ100`'s compile does not fit in wasm32's 4 GB.

### Sampling-smoke coverage

[`tools/check_corpus_sampling.py`](tools/check_corpus_sampling.py) runs the
fixtures with `sampling_smoke` metadata, currently the 13 Aalto models, for
100 warmup iterations and 100 saved draws, requiring finite values, complete
diagnostic columns, the exact reference output names and order, and the
expected draw count.

```sh
python3 tools/check_corpus_sampling.py --build build-rel
ctest --test-dir build-rel -R 'test_corpus_sampling|test_run_timings' --output-on-failure
```

## Unit tests for numerical operations

A kernel is the runtime implementation of one operation. Each CTest
constructs the call CmdStan's generated C++ would make with `stan::math::var`
and compares the value and every gradient component; the default assertion,
`expect_eq`, requires bitwise equality. The binaries are listed as
`STANLI_TESTS` in [`CMakeLists.txt`](CMakeLists.txt), and `test_capi` links
the in-tree shared library so it tests the same C ABI the packages use. A
regression test is first run against the pre-fix source and must fail for
the expected reason, and each test states the autodiff activity of its
reference arguments, since mixed data/variable instantiations can
reassociate.

Two tests check properties rather than values.
[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) replaces forward
value buffers with NaN between the sweeps, so any op on the
`backward_ignores_values` allowlist that reads them fails, and runs the
in-place, store-forwarding, rerolling, partitioning and island passes over
400 random graphs shaped like real models, comparing gradients before and
after. [`tests/graph_helpers.hpp`](tests/graph_helpers.hpp)'s `run_grad`
evaluates each call twice on one executor and aborts on any bit difference,
which catches state leaking between evaluations
([`runtime/src/constfold.cpp`](runtime/src/constfold.cpp) documents the case
that motivated it).

Fixture MIR is generated from checked-in `.stan` files with the pinned
compiler and never checked in; `./tools/gen_fixtures.sh` regenerates it and
`tools/gen_fixtures.py` owns the O0/O1 policy. `tools/dev_setup.sh` builds
the compiler both as the executable fixture generation runs and as the
in-process object `stanli_check` links, so tests compile through the
pipeline users run; builds without the object (Windows, ASan) use the
`stanli-compile` executable instead. `stanli_check --stanc PATH` runs an
external stanc for bisects.

Unit tests cover the cases they construct. They do not show that lowering
selects the intended kernel or that another path implements the same
operation; the next two sections do.

## Compiler producer parity

The `browser-compiler` job builds the shared OCaml pipeline as a native
executable and as js_of_ocaml, and
[`tests/test_portable_stancjs.cjs`](tests/test_portable_stancjs.cjs)
requires byte-for-byte compact-v2 equality on seven fixtures (an ordinary
model, nested UDFs, the mother model, an O1-folded literal, int32 overflow,
Unicode text, an in-memory include), plus repeat determinism, error and
warning parity, and the stock `stanc()` API against stock stancjs. The
Python, R and webR interface tests exercise filesystem includes through
every compiler route. After merge, the Windows jobs cross-build `stanc.exe`
and `stanli-compile.exe`, execute both, and run the same byte comparison.
`test_mir_decode` checks the typed producer's output on PRs, and post-submit
`bench_mir_decode` requires compact v2 to take at most half the legacy
decoder's median time and bytes on Eight Schools.

## Comparing stanli execution paths

Graph lowering, the MIR interpreter and the register program each map Stan
operations separately, so one can be wrong while another is right; matrix
division was once implemented in the interpreter and missing from lowering.

[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles one model once per
configuration and compares results: `STANLI_NO_ISLAND`,
`STANLI_ISLAND_ALWAYS`, `STANLI_NO_NATIVE_ADJ`, `STANLI_NO_ODE_DIRECT_RK`,
the graph passes off, and the shipped pipeline with `STANLI_WA_FORCE_INTERP`
running the interpreter beside a complete `write_array` graph. Stochastic
`write_array` columns, found by running the interpreter under two seeds, are
excluded. The gate is bitwise for every row; a divergence fails unless
[`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) declares it
with a bound and a reason, so a new 1 ULP difference is still reported. Four
entries exist: three operations the `write_array` graph supports and the
interpreter does not (`dirichlet_lpdf`, `ode_rk45`, `to_matrix`), and
`array[0] matrix[2, 3]`, which the interpreter rejects while the graph
returns a value. [`tests/test_cross_path.cpp`](tests/test_cross_path.cpp)
drives the matrix over every generated fixture and enforces minimum counts
per path, so a regression cannot hide as skips.

Generated adjoints and islands have focused gates. `gen_adjoint`
([`runtime/src/adjoint.cpp`](runtime/src/adjoint.cpp)) emits an island's
backward at load time, and [`tests/test_adjoint.cpp`](tests/test_adjoint.cpp)
compares it against replay under stan-math's `var`
(`STANLI_NO_NATIVE_ADJ=1`). `test_island` covers the proof that lets replayed
islands reuse executor-owned buffers (`STANLI_NO_REPLAY_REUSE=1` restores
fresh ones) and the constant and fill cleanups
(`STANLI_NO_DEAD_CONSTANTS=1`, `STANLI_NO_FILL_SINK=1`); `test_adjoint`
compares the three-lane softmax specialization
(`STANLI_NO_ISLAND_SOFTMAX3=1`) bit-for-bit against the canonical forward.

The direct RK path is gated bitwise with no ledger. `test_ode_prog` requires
derivative construction to leave the RHS bytecode unchanged and pins the
structural refusals; `test_odevariadic` compares the default path against a
separately lowered `STANLI_NO_ODE_DIRECT_RK=1` oracle for RK45, CKRK and
legacy RK45 on solution bits, Jacobian scratch, both pullbacks, whole-model
density and gradient, every activity mask, threaded execution and exception
text. An architecture-specific disagreement narrows eligibility and keeps the
gate.

## Testing graph transformations

Each group of passes has a diagnostic switch, listed in
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md).
[`harnesses/ab_corpus.py`](harnesses/ab_corpus.py) runs every posteriordb
model with selected passes off and on (by default rerolling, in-place
updates, constant folding and islands) and compares the log density and
every gradient component within 1e-11. It has caught pass errors in eight
models, the largest at 1.7e+05; the 0.9.1 note in
[`CHANGELOG.md`](CHANGELOG.md) records a whole-stack worst difference of
5.99e-13.

<a id="mir-loop-vectorization-measurement"></a>
[`harnesses/vectorize_ab.py`](harnesses/vectorize_ab.py) does the same for
the compiler's `vectorize_loops` pass: a test-only OCaml probe compiles each
of the 329 recorded models with the pass off and on, and every check reads
those exact MIR files through `stanli_check --mir`. Hard gates cover status
and category consistency, error parity, finite/NaN classes, shapes, names
and both modes against the CmdStan references; op counts and preparation
timings are diagnostics. The execution gate is gradient time on the fixed
`GRADIENT_MODELS` set: a model whose on/off ratio exceeds 1.04 is re-measured
in a fresh interleaved run and fails only if the re-run agrees. Both cells
share one runtime build, so a change that slows both equally is invisible
here. It runs after merge, nightly and on demand:

```sh
python3 harnesses/vectorize_ab.py deps/posteriordb \
  --compiler deps/stanc3/stanli-vectorize-probe \
  --check build-rel/stanli_check --bench build-rel/bench_grad \
  --dump build-rel/dump_ops --output-dir build-rel/vectorize-ab
```

## Testing Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py) reads every
signature the pinned compiler knows (`--dump-stan-math-signatures`) plus a
catalog of language constructs, generates a small type-correct model per
applicable case, and evaluates it with stanli (reading `--O1` MIR) and with a
reference BridgeStan build from the same pinned CmdStan, Stan, Math and stanc
sources. It compares log density and full gradient at fixed probe points,
10 ULP by default; reviewed exceptions live in
`harnesses/conformance/policy.toml` with a reason, and probe sums near zero
may use an absolute tolerance.

The baseline
([`docs/conformance-baseline.json.gz`](docs/conformance-baseline.json.gz))
holds 24,277 cases over 564 names: 18,750 `verified`, 3,662 `inapplicable`,
883 `generator_gap`, 695 `expected_unsupported` and 287
`unexpected_unsupported`. `mismatch`, `crashed` and `harness_error` block
the run. `unexpected_unsupported` means the reference accepted a case stanli
rejected; `expected_unsupported` is a reviewed policy boundary such as
complex or tuple results; `generator_gap` means the case was unusable as
evidence; `inapplicable` means no meaningful gradient case exists. Unsupported
cases and gaps are counted with reproduction commands but do not fail CI,
and a name in the inventory does not by itself mean any case verified.
Definitions are in
[`harnesses/conformance/status.py`](harnesses/conformance/status.py).

The sweep runs nightly in eight partitions from
[`.github/workflows/stan-conformance-nightly.yml`](.github/workflows/stan-conformance-nightly.yml),
then applies a ratchet: a case that was `verified` and no longer is fails
with `coverage_regressed:N`, a missing case or changed toolchain pin fails
as no longer comparable, and a newly verified case still carrying an
`expected_unsupported` rule reports `policy_improvements:N` until the rule
is removed. `--update-snapshot` rewrites the baseline for review with the
change. A separate `signature-watch` job reports new upstream signatures.

The sweep does not reach behavior used only through operators, cannot check
a case the generator cannot construct, and does not measure cost. [Ten integrated function models](tests/function_coverage/README.md)
close part of the first gap: 430 names, including data-only helpers, RNGs,
solvers and five operators, checked against recorded CmdStan values,
gradients and outputs on every PR. [`docs/coverage.md`](docs/coverage.md)
summarizes the inventory by function family. Local runs work on macOS after
`tools/dev_setup.sh --conformance`; see
[`harnesses/conformance/README.md`](harnesses/conformance/README.md).

## Detecting losses in model coverage

[`harnesses/model_census.py`](harnesses/model_census.py) applies the same
baseline idea to stanc3's 1,231 integration models, which cover syntax and
type combinations absent from posteriordb and the generator, comparing each
run with `docs/census-baseline.json.gz`. A model is `lowered` once stanli
evaluates its log density and gradient; leaving `lowered` fails by name with
a reproduction command, while movement among backlog states does not. With
`--differential`, the CmdStan comparison ranks `verified` above
`rejected_both` and disagreement. A model whose SHA-256 changes is not
compared. `crashed` and `timed_out` fail with or without a baseline;
`--update-baseline` rewrites it for review.

## Sampler configuration

Fixed-point tests cannot see sampler settings: when `run_nuts` failed to
call `set_max_depth`, trajectories were capped at 31 leapfrog steps instead
of 1023 while every gradient comparison passed.
[`tools/sampler_trace.py`](tools/sampler_trace.py) runs the same model,
seed, warmup and sampling length through both engines and compares total
leapfrog steps, maximum tree depth, final step size, divergence rate and
scaled mean `lp__`; the tolerances catch large errors such as a 30-fold
leapfrog difference. Use its default adaptation target of 0.8, because
`--delta` is forwarded to CmdStan only. The executor also counts gradient
evaluations (`n_grad_evals()`), and
[`tests/test_sampling.cpp`](tests/test_sampling.cpp) uses the counter to
require `ar1` to average more than 40 leapfrog steps per iteration.

## Building without stdio

The R package stanr vendors this runtime into a CRAN-style package, and
`R CMD check` rejects compiled code referencing stdout, stderr, `printf`,
`puts`, `abort`, `exit` or the assert failure path.
[`tools/check_no_stdio.sh`](tools/check_no_stdio.sh) builds the runtime
object library with `-DSTANLI_NO_STDIO` and scans every object's undefined
symbols for that list; the `no-stdio` job runs it after merge.

## Memory safety

Memory errors can produce finite wrong answers: one GLM case returned -29.48
where the equivalent array form returned -16.22, from a four-byte read past
a 12-byte allocation. The `asan` job runs the complete CTest suite
instrumented after merge and nightly, since a cold instrumented build
approaches the 90-minute limit. Locally:

```sh
asan_jobs=$(STANLI_JOB_MEMORY_GIB=12 tools/build_jobs.sh)
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTANLI_SANITIZE=address
cmake --build build-asan --parallel "$asan_jobs" && \
  ctest --test-dir build-asan --parallel "$asan_jobs"
```

ASan does not catch every lifetime error in stan-math's arena, where a
stale `vari` can point into memory already reused by a live allocation;
repeat-evaluation tests cover that case.

## Checks run before and after merge

PRs use one representative native build and one complete external oracle;
broad sweeps and platform matrices run after merge. The source-change path
in [`.github/workflows/wheels.yml`](.github/workflows/wheels.yml) and
[`.github/workflows/lint.yml`](.github/workflows/lint.yml) is:

- Static checks: generated documentation, development-setup contracts,
  browser syntax, Windows exports, CI routing and C/C++ formatting.
- One Linux x86_64 Clang build: the full CTest suite and every recorded
  CmdStan model at all three points.
- Installed wheel checks: Python and BridgeStan transport tests in a clean
  environment, manylinux tags and the binary-size artifact.
- Shared compiler checks: producer parity, MIR decoding, provenance and
  demo-model metadata.
- One Linux R integration job on that runtime: `R CMD check`, compilation
  through V8 and ecosystem acceptance with skips treated as failures.

PRs limited to allowlisted Markdown and license files, `AGENTS.md`,
research under `notes/` or `web/index.html` run only the static checks;
executable code and fixtures must not live under `notes/`. The required
`manylinux_2_28_x86_64` status is present for every PR and cannot turn
green on a failed, cancelled or unexpectedly skipped prerequisite; unknown
paths take the full path.

Main pushes, nightly runs, release tags and manual dispatches add the
vectorization A/B, MIR cost measurements, live BridgeStan comparisons,
cross-release R compatibility, first-posterior timings, no-stdio, Windows
compiler parity and the full Windows wheel, the other native platforms,
WebAssembly and webR. ASan and TSan run on the same events except release
tags. A change that needs that evidence before landing dispatches the
workflow on its branch:

```sh
gh workflow run wheels.yml --ref my-change
```

Release tags additionally assert that the version in
`python/stanli/__init__.py`, `js/package.json` and `r/R/install.R` matches
the tag, that six runtime tarballs exist and that the PyPI description
renders, and call [`.github/workflows/stanr.yml`](.github/workflows/stanr.yml)
to rebuild downstream `stanr` with the tagged runtime before any publisher
runs.

Every headline number in `README.md`, `python/README.md` and the demo page
is stamped from `docs/verification.json` and the current benchmark
artifacts by [`tools/gen_docs.py`](tools/gen_docs.py); `--check` fails CI
when a stamped number disagrees with its artifact.

## Known limits

Two of the 120 posteriordb models are not verified. `sir`'s ODE solution
falls below a declared lower bound at every shared point and both engines
reject it. `kronecker_gp` matches the log density to 1e-13 and 436 of 438
gradients, differing by 0.7% on the two that flow through
`eigenvectors_sym` of a nearly degenerate covariance. Both are written up in
[`docs/corpus-status.md`](docs/corpus-status.md).

Transformed data has one engine, so every cross-path configuration shares
the MIR interpreter's answer there and that section is covered only on the
CmdStan axis.

Reduction order is not frozen across releases. Fusion and packet arithmetic
can move some reductions by their last bits within the policy budgets;
releases that do so carry a compatibility note in
[`CHANGELOG.md`](CHANGELOG.md) with the worst measured deviation.

Some less common multivariate and multinomial densities match CmdStan's
gradients but report `lp__` with a parameter-independent offset, which
leaves the posterior unchanged and can change a pinned-seed trajectory. The
optional `STANLI_LITE_LP` build makes the same trade across the whole
library; it is off in every shipped build, `stanli_exact_lp()` reports which
is loaded, and it is checked by hand with `tools/verify_lite.py`; CI does not
cover it because that would need a second full stan-math compile. See
[`docs/lp-constant.md`](docs/lp-constant.md).

The conformance backlog is 287 `unexpected_unsupported` cases, each with a
reproduction command. The cross-path ledger has four open entries, one of
which records a known graph error.

## Reproducing this locally

Replaying the checked-in references does not run CmdStan; the setup script
prepares CmdStan because the recording and sampler tools use it:

```sh
./tools/dev_setup.sh --corpus     # native build, posteriordb, and CmdStan
python3 tools/verify_refs.py deps/posteriordb \
  --check build/stanli_check --jobs 8
ctest --test-dir build --parallel "$(tools/build_jobs.sh)"
```

The A/B and cross-path commands use release-build tools outside the setup
script's default targets:

```sh
./tools/dev_setup.sh --conformance   # the reference stack for the sweep
cmake --build build-rel --parallel "$(tools/build_jobs.sh)" \
  --target stanli_check dump_ops
python3 harnesses/ab_corpus.py deps/posteriordb            # pass A/B
build-rel/stanli_check model.stan data.json --cross        # cross-path
python3 tools/verify_refs.py deps/posteriordb --wa-report  # GQ coverage
python3 tools/verify_refs.py deps/posteriordb --wa-headers deps/cmdstan
python3 tools/sampler_trace.py \
  deps/cmdstan deps/posteriordb eight_schools_noncentered
python3 harnesses/model_census.py --differential \
  --cmdstan deps/cmdstan --jobs 10
```

The WebAssembly replay also needs Node, Emscripten and the stancjs
dependencies CI uses:

```sh
./tools/build_web.sh
python3 tools/verify_refs.py deps/posteriordb \
  --check tools/wasm_check.sh --skip nn_rbm1bJ100
```

`./tools/dev_setup.sh --all` prepares the native corpus and conformance
dependencies without Emscripten. [`docs/hacking.md`](docs/hacking.md) has the
per-change recipes, and [`docs/how-it-works.md`](docs/how-it-works.md)
describes the design these numbers check.
