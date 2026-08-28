# Why should I trust this?

stanli is a reimplementation of the Stan runtime. Confidence in a
numerical implementation should be based on reproducible comparisons and
clear limits. This page describes what is tested, what each test can show,
and where it runs. It also links each claim to the code or artifact that
supports it.

These tests do not establish correctness for every Stan program or every
possible input. They provide evidence in two complementary ways.

First, stanli is compared with CmdStan. Both runtimes evaluate the same
model and data at the same fixed points in the unconstrained parameter
space. These are test inputs, not draws from a posterior; agreement at three
points does not establish agreement over the whole parameter space. The
comparison includes the log density, every component of its gradient, and,
where available, every output produced for a draw: constrained parameters,
transformed parameters, and generated quantities. A separate suite inventories
24,277 function signatures and language constructs that are uncommon in
applied-model corpora; 18,750 cases are currently classified `verified`. The
reference values are produced by running the pinned Stan toolchain; they are
not hand-written. Sampler behavior is tested separately because it is not
determined by a single pointwise gradient evaluation.

Second, stanli's execution paths are compared with one another. The runtime
can evaluate parts of a model with an operation graph, a middle-level
intermediate representation (MIR) interpreter, or a register machine, and it
applies several graph transformations. Comparing these configurations helps
detect implementation errors that a single path could miss. The cross-path
tests require the configurations to agree bitwise. The regular corpus test
compares the shipped, optimized graph with CmdStan; a separate A/B test
compares that graph with versions in which selected optimizations are
disabled.

The numerical criterion depends on the comparison. Unit tests for individual
operations and the cross-path tests use bitwise equality by default. A small
number of operations have a documented allowance of at most 2 ULP (units in
the last place) because equivalent floating-point evaluation orders can
differ in their final bits. Most corpus points use a 1e-9 gate on the scaled
error `|a-b| / max(|a|, |b|, 1)`. Three documented `kronecker_gp` points use
limits based on their measured deviations, and points rejected by CmdStan
require matching rejection behavior. The sections below give the details and
known exceptions.

## Overview of the checks

| check | question | acceptance rule | schedule |
| --- | --- | --- | --- |
| unit tests for numerical operations | Does one numerical operation or graph transformation agree with stan-math? | Bitwise by default; a documented 2 ULP limit for selected paths | every pull request |
| compiler producer parity | Do native OCaml, js_of_ocaml, and the Windows executable emit identical compact-v2 bytes while the stock rollback paths remain usable? | Byte-for-byte identity on seven successful models; JS API/error/warning/rollback checks; Windows provenance, executable-format, and final-newline checks | every pull request |
| MIR wire cost | Is the compact-v2 decoder materially faster and the wire materially smaller than legacy MIR? | On Eight Schools, median decode time and raw bytes must each be at most half the legacy value | every pull request |
| corpus comparison | Are 119 posteriordb models and 11 compiler-derived fixtures consistent with recorded CmdStan behavior at three fixed inputs? | Scaled error of 1e-9 for most points; documented limits for three `kronecker_gp` points; rejection parity | every pull request |
| cross-path matrix | Do stanli's execution paths agree with one another? | Bitwise, except entries named in the ledger | every pull request, within CTest |
| transformation A/B | Do selected graph optimizations preserve model results? | Optimizations enabled and disabled agree at the default point within 1e-11 | manually after optimization changes |
| BridgeStan C-ABI comparison | Does the public C interface agree with reference BridgeStan? | Four fixture models must pass value, name, count, and output-shape checks | every pull request |
| generated conformance sweep | For cases that both systems evaluate, do results agree, and which generated cases remain unsupported? | 10 ULP by default; reviewed per-case policy where needed | nightly and on demand |
| coverage baseline | Did a previously verified generated case stop verifying? | No loss of a verified case; obsolete policy exceptions must be removed | within the nightly sweep |
| model census | Do stanc3's 1,231 integration models still compile into stanli's runtime representation and, where checked, agree with CmdStan? | No decrease in per-model classification | manually, on demand |
| sampler trace | Is NUTS configured comparably to CmdStan? | Diagnostic summaries remain within limits chosen for large configuration errors | manually after sampler changes |
| AddressSanitizer | Does ASan detect invalid memory access while CTest runs? | No sanitizer diagnostics | after merge and nightly |
| WebAssembly replay | Does the browser runtime reproduce the recorded corpus? | Same numerical gates; 118 of 119 compiling posteriordb models fit in wasm32 | manually |
| documentation and formatting | Do generated claims match their artifacts, and is C/C++ formatting current? | Exact generated-file and formatter checks | every pull request |

## MIR loop-vectorization measurement

[`harnesses/vectorize_ab.py`](harnesses/vectorize_ab.py) measures one source
MIR pass against an explicit pass-off oracle. It selects upstream
`vectorize_loops` by default; `--candidate-pass
distribute-same-lane-density-loops` selects the Stanli-owned loop rewrite.
The test-only OCaml probe compiles each source once with the candidate off and
once with it on; every later check consumes those exact portable MIR files
through `stanli_check --mir`. All other source-pass choices are explicit and
identical between the two cells.

The complete run covers all 130 recorded models plus any posteriordb census
model without a recorded CmdStan row:

```sh
python3 harnesses/vectorize_ab.py deps/posteriordb \
  --compiler deps/stanc3/stanli-vectorize-probe \
  --check build-rel/stanli_check --bench build-rel/bench_grad \
  --dump build-rel/dump_ops --output-dir build-rel/vectorize-ab
```

CI runs the bounded seven-model selection shown by the "Bounded MIR
vectorization A/B" workflow step. It includes three models whose MIR must
change and four controls. The hard gates cover command/status consistency,
result and write-array categories, error parity, per-element finite/NaN/
infinity classes, shapes and names, and both pass modes against the existing
CmdStan references. Finite bit differences that remain within those gates are
listed separately with bit patterns and ULP distances.

The output directory contains `manifest.json`, `corpus.jsonl`,
`graphs.jsonl`, `bench.tsv`, `summary.json`, and `summary.md`. The manifest
records source pins, producer provenance, tool hashes, platform, toolchain,
environment policy, and corpus scope. Compiler wall time, graph and
preparation counts, separate log-density/write-array reroll dispositions, and
auto-calibrated ABBA gradient timings are descriptive measurements; their
ratios do not decide pass/fail. Missing or malformed measurement output does
fail the run because it would make the report incomplete. The report labels
CmdStan-referenced and A/B-only models separately; the latter have off/on
category, error, shape, name, and value parity but no fabricated reference
gate. [`tools/corpus.py`](tools/corpus.py) remains the source-only census
report.

## Comparison with CmdStan on complete models

[`tools/verify_refs.py`](tools/verify_refs.py) replays every referenced
posteriordb model, plus the language-construct models in
[`tests/stanc3/`](tests/stanc3/), against CmdStan's recorded log density
and full unconstrained gradient. This is the broadest whole-model numerical
comparison in the repository. It has found errors that isolated kernel tests
did not reach, which is why it runs in addition to the unit-test suite.

The reference artifact
[`docs/corpus-refs.json.gz`](docs/corpus-refs.json.gz) contains:

- 130 models at 3 deterministic unconstrained points each, 390 points in
  total, holding 348,511 log-density and gradient values. The models are
  119 posteriordb models that evaluate and 11 language fixtures adapted
  from stanc3's compiler tests.
- Every value is the exact `%.17g` string CmdStan's driver printed
  ([`tools/ref_driver.cpp`](tools/ref_driver.cpp)), so the replay
  compares against the bits CmdStan produced rather than a rounded copy.
- 109 models carry at least one complete row from Stan's per-draw output
  routine, `write_array`, at the same points: 324 rows and 455,451 values
  covering constrained parameters, transformed parameters, and generated
  quantities. Column names are also compared exactly. Both direct
  `write_array` drivers start Stan's RNG with
  seed 1234 and chain 0, so generated-quantity draws are reproducible in
  this comparison. This is a controlled pointwise test; it does not claim
  to reproduce the RNG state after a CmdStan sampling run. The recorder stores
  an output row only after stanli matches its names, width, and values, so an
  absent row means that output is not covered by this comparison. The
  primary-point counts for posteriordb models are in
  [`docs/corpus-status.md`](docs/corpus-status.md); the aggregate above also
  includes the stanc3 fixtures and all three points.
- Reference provenance recorded in the file: CmdStan
  2.39.0 at `11cb052d`, Stan `c96d0411`, Math `8f326d14`, stanc3
  `ac69570a`, posteriordb `28f8d3d6`, on Darwin arm64.

The references were recorded by
[`tools/verify_sample.py`](tools/verify_sample.py) against that CmdStan
checkout, compiled with `-ffp-contract=off` to match CmdStan's own build
flags. stanli sets the same flag on stan-math project-wide
([`CMakeLists.txt`](CMakeLists.txt), line 113). Both sides use Stan's sampling
log density (`propto=true`) and include the unconstrained-to-constrained
Jacobian adjustment. Parameter-independent normalizing constants may therefore
be omitted, but the two runtimes are compared under the same convention.
Every point is recorded, including points CmdStan refuses. Recording all three
points matters because different points can exercise different branches of a
model; an earlier version of the recorder retained only the first point
accepted by both runtimes.

For verified points, the replay gate is 1e-9 on the scaled error
`|a-b| / max(|a|, |b|, 1)`, compared with 1e-10 when references are generated.
The wider replay gate accounts for small differences between Apple's system
math library and the libraries used on other platforms. Known implementation
errors detected by this comparison were much larger; for example, one
in-place update error produced a scaled difference of 1.7e+05.

Of the 390 recorded points, 384 have status `VERIFIED`, 5 have status
`MISMATCH`, and 1 has status `REJECTED_BOTH`. Two `accel_gp` points retain
their recorded `MISMATCH` labels from before a zero-adjoint error was fixed;
the current replay agrees with their references. The other three belong to
`kronecker_gp`. Two of that model's eigenvector gradients are sensitive to
a nearly degenerate covariance whose smallest eigenvalue gap is 6.5e-17.

Verified points and the two historical `accel_gp` labels use the standard
1e-9 gate. The three `kronecker_gp` points use limits derived from their
recorded deviations and measured cross-platform variation. No recorded point
is currently excluded from enforcement. This keeps the known numerical
limitation visible without disabling checks for other models.

In CI: the step "Corpus verification against CmdStan references" in
[`.github/workflows/wheels.yml`](.github/workflows/wheels.yml), on every
pull request, every push to `main`, and nightly.
[`tools/wasm_check.sh`](tools/wasm_check.sh) drives the same replay
through the WebAssembly build, where 118 of the 119 compiling corpus
models pass. `nn_rbm1bJ100` is the exception: its compile does not fit in
wasm32's 4 GB.

This comparison covers complete models, but it does not cover every Stan
function or type signature. The generated conformance sweep addresses that
separate question.

## How numerical agreement is measured

[`docs/corpus-status.md`](docs/corpus-status.md) publishes the worst
deviation for each posteriordb model as both scaled error and ULPs. A ULP is
one step between adjacent representable floating-point numbers at a given
magnitude. At their primary recorded point, 41 verified posteriordb models
have 0 ULP difference: their log density and every gradient component are
bit-identical to CmdStan. Eight additional language fixtures have 0 ULP
difference in [`docs/verification.json`](docs/verification.json).

Close agreement is expected because stanli and CmdStan both call stan-math
and use the same floating-point compiler flags. stanli's native kernels also
follow stan-math's reverse-mode expressions. Evaluation order still matters:
floating-point addition is not associative, so `(a+b)+c` can differ from
`a+(b+c)` in the last bit. For this reason, kernels preserve the reference
summation order where practical. The contract in
[`docs/hacking.md`](docs/hacking.md) gives a concrete example: reordering a
matrix-vector sum remains within the corpus's 1e-9 scaled-error gate but changes
the result by 1 ULP, which [`tests/test_matvec.cpp`](tests/test_matvec.cpp)
detects.

Kernel paths with an explicit ULP allowance are documented at their call
sites. For example, the elementwise `log` kernel uses Eigen's packet
implementation, which can differ from the system math library by 1 ULP on
some inputs. The corresponding packet `exp` path is not used because its
accumulated effect moves
`kronecker_gp` outside its measured reference gate
([`runtime/kernels/eltwise_expr.cpp`](runtime/kernels/eltwise_expr.cpp),
line 333). Several other Eigen expressions have documented differences of
1-2 ULP because they reassociate arithmetic.

Any non-bitwise limit is tied to a measured difference and a documented
mechanism.

## Unit tests for numerical operations

The runtime implementation of an individual numerical operation is called a
kernel. The CTest suite compares these kernels with stan-math. A test
constructs the call that CmdStan's generated C++ would make using
`stan::math::var`, then compares the value and every gradient component. The
default assertion, `expect_eq`, requires bitwise equality. The C++ test
binaries are listed as `STANLI_TESTS` in
[`CMakeLists.txt`](CMakeLists.txt). `test_capi` is linked against the in-tree
shared-library target so that it tests the same C ABI boundary used by client
packages.

Two testing practices are used consistently:

- A regression test is run against the pre-fix source and must fail for the
  expected reason. This confirms that the test distinguishes the faulty and
  corrected implementations.
- Tests specify the automatic-differentiation activity of each reference
  argument. A reference with the same active variables can be compared
  bitwise. CmdStan's mixed data/variable instantiations may reassociate
  expressions and differ by several ULP, so tests state which form they use
  and why.

Two notable tests check properties rather than individual expected values.

[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) checks the
`backward_ignores_values` allowlist. An operation may appear on this list only
if its reverse pass does not read the forward-pass value buffers, which then
allows those buffers to be reused. The test replaces the buffers with NaN
between the forward and reverse sweeps; an operation that incorrectly reads
them will produce NaN adjoints and fail. The same file applies the in-place,
store-forwarding, rerolling, partitioning, and island transformations to 400
randomly generated graphs with shapes drawn from real models, then compares
gradients before and after transformation.

[`tests/graph_helpers.hpp`](tests/graph_helpers.hpp)'s `run_grad` evaluates
each invocation twice on the same executor and aborts on any bit difference.
Pass tests that use this helper therefore check repeatability within one
process. It was added after constant folding turned a `rep_vector` base into
a bind-time fill and later in-place writes accumulated across evaluations.
The same input then produced different log densities on repeated calls. The
guard that prevents that transformation is documented in
[`runtime/src/constfold.cpp`](runtime/src/constfold.cpp). A single-evaluation
test would not detect this state leak.

Fixture MIR is generated from checked-in `.stan` files with the pinned
compiler:

```sh
./deps/stanc3/stanc --O1 --debug-optimized-mir tests/fixtures/X.stan \
  > tests/fixtures/X.tmir.sexp
```

The `.hpp` file that stanc also writes beside the model is not part of the
fixture and is removed.

`tools/dev_setup.sh --embed` and `--corpus` build the compiler executable from
the configured stanc3 source revision. Core setup intentionally omits a
compiler.

Unit tests check the kernels and cases they explicitly construct. They do not
by themselves establish that model lowering selects the intended kernel or
that another execution path implements the same operation.

Portable MIR has separate producer and decoder gates. The `browser-compiler`
CI job builds the shared OCaml pipeline once as both a native executable and
js_of_ocaml, then
[`tests/test_portable_stancjs.cjs`](tests/test_portable_stancjs.cjs) requires
byte-for-byte compact-v2 equality for an ordinary model, nested UDFs, the
mother model, an O1-folded binary64 literal, checked int32 overflow/no-fold
behavior, Unicode text, and an in-memory include.
It also checks repeat determinism, source-bearing error objects, warning
parity, the normal `stanc()` JavaScript API against stock stancjs on ordinary
inputs, and final-newline behavior. The overflow fixture is excluded from that
API comparison because the checked producer policy deliberately retains an
expression where pristine stanc3's host-width-dependent folds disagree. A
focused worker harness proves the preferred custom
import and the missing-artifact fallback import. The tested JavaScript
artifacts are the ones consumed by the Pages and npm jobs. The manylinux gate
then decodes the same typed-producer output directly with `bench_mir_decode`.
For Eight Schools, compact v2 must take no more than half the legacy decoder's
median time across 51 repetitions and no more than half its raw bytes. Gzip and
complete preparation timings remain descriptive measurements in the uploaded
artifact.

The same gate covers the Windows producer on every pull request. The
`stanc-windows` job cross-builds pristine `stanc.exe` before applying the
stanli overlay, then cross-builds `stanli-compile.exe` and records its source
and core-toolchain stamp. The `windows-compiler` job executes both PE
artifacts on Windows, proves the stock overflow behavior remains pristine, and
runs the seven-model byte comparison above between `stanli-compile.exe` and
the JavaScript producer. The surrounding JavaScript suite separately checks
errors, warnings, and its stock API. A real R subprocess check then runs both
executables from paths containing spaces and Unicode, stages CRLF source as
UTF-8 bytes, and checks portable versus legacy envelopes. This bounded gate
builds no stan-math runtime or Windows wheel.

## Comparing stanli execution paths

A CmdStan comparison exercises the stanli path selected for that model and
configuration. It does not show that another stanli execution path gives the
same result. This matters because graph lowering, the MIR interpreter, and
the register program have separate mappings from Stan operations to runtime
operations. For example, matrix division was once implemented in the MIR
interpreter but missing from graph lowering.

[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles one model several
times, once per configuration, and compares the results. The
configurations are `STANLI_NO_ISLAND`, `STANLI_ISLAND_ALWAYS`,
`STANLI_NO_NATIVE_ADJ`, rerolling/in-place updates/constant folding off, and
the shipped pipeline with `STANLI_WA_FORCE_INTERP` attaching `WaInterp` beside
a complete `write_array` graph so both engines produce the same output row.

The cross-path result excludes stochastic `write_array` columns from its
bitwise count. It identifies them by running the interpreter with two seeds
and checking which values change. Where a complete-model `write_array` row is
recorded, the CmdStan comparison above tests those columns separately with
matched seed and chain settings.

The gate is bitwise for every row. A divergence fails unless
[`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) declares
it with a bound and an explanation. Ledger entries remain explicit rather
than becoming a general tolerance, so a new 1 ULP difference is still
reported. Four entries are currently recorded. Three are operations that
the `write_array` graph supports but the interpreter does not
(`dirichlet_lpdf`, `ode_rk45`, and `to_matrix`). The fourth records a known
difference for `array[0] matrix[2, 3]`: the interpreter rejects the input
while the graph returns a value.

[`tests/test_cross_path.cpp`](tests/test_cross_path.cpp) drives the matrix
over every `tests/fixtures/*.tmir.sexp` file (146 currently), so a new fixture
is included automatically. Fixtures that cannot compile or evaluate are
recorded as skipped. Minimum counts are enforced for each execution path and
output mode, preventing a broad regression from appearing only as additional
skips. The exact current thresholds are kept beside the test.

Generated adjoints have a separate comparison. `gen_adjoint`
([`runtime/src/adjoint.cpp`](runtime/src/adjoint.cpp)) generates an
island's backward as a second instruction list at load time;
`STANLI_NO_NATIVE_ADJ=1` restores the replay under stan-math's `var`,
which is what [`tests/test_adjoint.cpp`](tests/test_adjoint.cpp) checks
the generated program against.

Native-adjoint islands can additionally select the shared three-lane softmax
forward specialization. `STANLI_NO_ISLAND_SOFTMAX3=1` leaves the admitted
island and its generated backward intact but uses the canonical forward
program, giving a focused A/B switch. `test_adjoint` compares the specialized
and canonical forward register files bit-for-bit, checks the generated
backward, and covers the activation threshold, clone budget, overlap, and NaN
behavior. `test_island` covers the opt-out through graph carving and execution,
tagged-payload lifetime, and the rule that opcode zero remains invalid for
graph carving and binding after the private helper is registered.

## Testing graph transformations

Each group of graph transformations has a diagnostic environment switch.
These switches allow a result or performance change to be associated with one
group while keeping the executable otherwise unchanged.
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) lists the
transformations and their switches.

[`harnesses/ab_corpus.py`](harnesses/ab_corpus.py) attempts every posteriordb
model with selected transformations disabled and enabled, then compares the
log density and every gradient component when both configurations evaluate.
By default it disables rerolling, in-place updates, constant folding, and
islands; `--disable` can select a different set. There are therefore two
related comparisons at the default test point: the ordinary corpus run
compares the shipped graph with CmdStan, and this A/B run compares the shipped
graph with selected optimizations disabled. The A/B limit is 1e-11 on log
density and every gradient component. Different compile or evaluation
statuses are reported. If both configurations return the same leading failure
status, the harness counts a known gap without comparing the failure details.
This test has detected optimization errors affecting eight models, with a
largest relative difference of 1.7e+05.

The 0.9.1 compatibility note in [`CHANGELOG.md`](CHANGELOG.md) records the
result for the complete pass stack. At those points, the largest difference
between graphs with the selected optimizations enabled and disabled was
5.99e-13.

## Testing Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py)
reads every function signature known to the pinned stanc compiler
(`--dump-stan-math-signatures`) and a catalog of language constructs. It
attempts to generate a small, type-correct model for each applicable case,
then evaluates successful cases with stanli and with a reference BridgeStan
build made from the same pinned CmdStan, Stan, Math, and stanc sources. Cases
that cannot be generated are reported separately. stanli reads the compiler's
`--O1` MIR, while BridgeStan compiles C++ produced by the same compiler and
optimization setting. The comparison includes log density and the complete
gradient at fixed probe points. The default gate is 10 ULP; reviewed
exceptions are recorded in `harnesses/conformance/policy.toml` with a reason,
and unused policy rules are reported. Probe sums that approach zero can use an
absolute tolerance because relative error and ULP counts are not informative
near cancellation.

The checked-in inventory
([`docs/conformance-baseline.json.gz`](docs/conformance-baseline.json.gz))
holds 24,277 cases over 564 function names: 24,246 signatures and 31
language constructs. 18,750 verified, 3,662 inapplicable, 883
`generator_gap`, 695 `expected_unsupported`, 287
`unexpected_unsupported`.

Each case receives one status. `verified` means the generated case passed its
comparison. `mismatch`, `crashed`, and `harness_error` block the run.
`unexpected_unsupported` means the reference accepted the case while stanli
rejected it; many of these rows identify unimplemented signatures.
`expected_unsupported` marks the same outcome as a reviewed policy boundary,
such as complex or tuple results. `generator_gap` means the generated case was
not usable as evidence—for example, the reference rejected a nominally valid
case or its gradients were uninformative. `inapplicable` means there was no
meaningful real-valued case to test. The definitions are in
[`harnesses/conformance/status.py`](harnesses/conformance/status.py).

Unsupported cases and generator gaps are reported, counted, and given
reproduction commands, but do not by themselves fail CI. The coverage
baseline described below prevents a previously verified case from becoming
unsupported. Results are reported per generated signature or language case;
the appearance of a function name in the inventory does not by itself show
that any of its cases were numerically verified. Each run also writes
`unsupported.md`, grouping the backlog by function.

It runs nightly in eight partitions from
[`.github/workflows/stan-conformance-nightly.yml`](.github/workflows/stan-conformance-nightly.yml),
with an aggregate job that rejects missing or duplicate rows before
applying the ratchet. Local runs are supported on macOS; the scheduled CI
partitions run on Linux:

```sh
tools/dev_setup.sh --conformance          # one-time setup, idempotent
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python \
  --filter beta_binomial --output /tmp/conf
```

Use the virtual-environment interpreter so that the required TOML parser is
available. A filtered run reports `partial_run` by design. After rebuilding
the runtime, copy the new library into `python/stanli/_bin/` before running the
sweep; otherwise the Python package will load the previous build.
[`harnesses/conformance/README.md`](harnesses/conformance/README.md) has
the rest.

The generated sweep has three main limits. It does not cover behavior reached
only through operators rather than function calls; for example, an earlier
implementation of `B / A` as elementwise division did not create a failing
function-signature case. It cannot check a case that the generator cannot
construct (`generator_gap`). It also measures agreement, not execution cost.

[`docs/coverage.md`](docs/coverage.md) contains a function-family summary and a
script for regenerating it from an aggregate run; the nightly workflow does
not update that checked-in page automatically. A separate nightly job,
`signature-watch`, compares the pinned signature list with stanc3 upstream and
reports new signatures. Advancing the compiler pin reruns the complete
inventory.

## Detecting losses in language coverage

Because an unimplemented generated case is non-blocking, the nightly also
compares each case with `docs/conformance-baseline.json.gz`. A case that was
verified and is no longer verified fails with `coverage_regressed:N` and
lists the affected IDs. A missing case or changed toolchain pin also fails
because the new run is no longer directly comparable with the baseline. A
newly verified case normally does not fail. If it still carries an
`expected_unsupported` policy rule, the run reports `policy_improvements:N`
until that obsolete exception is removed.

Updating the baseline requires an explicit `--update-snapshot` run, and the
result is reviewed with the implementation change.

## Detecting losses in model coverage

The model census applies the same baseline approach to complete models rather
than generated function signatures. It covers syntax and type combinations
that may be absent from both the posteriordb corpus and the signature
generator. For example, issue #145 made `size()` of a scalar `int` fail to
compile. The census included that form, but its earlier aggregate count did
not identify the affected model. Per-model baselines were added in #151.

[`harnesses/model_census.py`](harnesses/model_census.py) compares each run
with `docs/census-baseline.json.gz` by default across stanc3's 1,231
integration models. In the census terminology, a model is `lowered` only after
stanli translates it into the runtime representation and successfully
evaluates its log density and gradient. The census uses two ordered
classifications:

- The lowering classification places `lowered` above the six backlog states
  (`unsupported`, `data_rejected`, ...) above `crashed`/`timed_out`/
  `harness_error`. A model moving out of `lowered` fails by name and prints a
  reproduction command. Movement among backlog states does not fail because
  those distinctions depend partly on the harness's classification rules.
- For models that lower, the CmdStan comparison places `verified` above
  `rejected_both` and disagreement. This ordering is used only when both runs
  produced a differential verdict. A census run without `--differential`
  still checks lowering coverage without treating absent differential results
  as regressions.

If a model's own SHA-256 changes, its previous result is not compared because
it refers to different source. A change to generated-data SHA-256 remains
visible and annotated. Generated data are cached by model hash; otherwise a
cold checkout could invalidate the baseline for every model at once.

`crashed` and `timed_out` fail with or without a baseline. To update the
baseline, rerun with `--update-baseline` and review the changed file with the
implementation change.

## Sampler configuration

The tests above evaluate models at fixed points. They do not test sampler
configuration because settings such as maximum tree depth do not enter a
pointwise gradient. This distinction mattered when `run_nuts` did not call
`set_max_depth`: trajectories were limited to 31 leapfrog steps rather than
1023 even though every fixed-point gradient comparison still passed.

[`tools/sampler_trace.py`](tools/sampler_trace.py) checks this class of
behavior. It runs the same model and data with the same seed, warmup length,
and sampling length, then compares summaries of the diagnostic columns from
stanli and CmdStan. The two samplers do not remain on the same trajectory,
so individual draws are not compared. Instead, the script compares total
leapfrog steps, maximum tree depth, final adapted step size, divergence rate,
and mean `lp__` scaled by the larger empirical standard deviation. Its
tolerances are intended to detect large configuration errors such as a
30-fold leapfrog difference or an order-of-magnitude step-size difference;
this is not a general statistical validation of NUTS.

The current script should be used with its default adaptation target of 0.8.
Its nondefault `--delta` option is forwarded to CmdStan but not to
`stanli_run`, so a nondefault value does not produce a matched-configuration
comparison.

The executor counts gradient evaluations (`n_grad_evals()`). For a one-chain
run, `stanli_run` prints that count, so sampling time can be separated into
cost per gradient and the number of gradients requested by the sampler. In a
multi-chain run, the current message reports only the first chain's executor,
not a total across chains. These quantities measure different sources of
runtime cost and can move independently.
[`tests/test_sampling.cpp`](tests/test_sampling.cpp) uses the same
counter in a regression test for maximum tree depth: `ar1` must average more
than 40 leapfrog steps per iteration, which cannot occur with a depth-5 limit
of 31.

## Memory safety

Memory errors do not always terminate the process; they can also produce
finite but incorrect results. In one GLM case, mapping `rows` integer
outcomes from a three-element group returned -29.48 while the equivalent
array form returned -16.22. AddressSanitizer identified an out-of-bounds
four-byte read immediately after a 12-byte allocation in
`poisson_log_glm_lpmf`.

The `asan` job in [`.github/workflows/wheels.yml`](.github/workflows/wheels.yml)
runs the complete CTest suite with AddressSanitizer instrumentation. It runs
after merges and nightly rather than on pull requests because a cold
instrumented build can approach the workflow's 90-minute limit. Locally it
is enabled with one CMake option:

```sh
asan_jobs=$(STANLI_JOB_MEMORY_GIB=12 tools/build_jobs.sh)
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTANLI_SANITIZE=address
cmake --build build-asan --parallel "$asan_jobs" && \
  ctest --test-dir build-asan --parallel "$asan_jobs"
```

CI uses AddressSanitizer alone. `STANLI_SANITIZE=address,undefined` is also
available for local diagnostics but is not part of the current CI result.
AddressSanitizer does not detect every lifetime error in stan-math's arena
allocator. A stale `vari` may point into memory that has already been reused
for another live allocation, which is still addressable. Numerical and
repeat-evaluation tests are needed for that case.

## Checks run before and after merge

The full source-change path in the pull-request workflows requires the
following checks, as defined in
[`.github/workflows/wheels.yml`](.github/workflows/wheels.yml) and
[`.github/workflows/lint.yml`](.github/workflows/lint.yml):

- one platform build, `manylinux_2_28_x86_64`;
- the compiler-only Windows cross-build and executable parity gate described
  above;
- the full ctest suite, which includes the cross-path matrix and the
  pass-safety fuzz;
- the CmdStan corpus comparison at all three points;
- `tools/gen_docs.py --check`;
- `tools/gen_web_models.py --check`, which requires every demo model to
  carry a hand-written description;
- the manylinux tag check: the highest `GLIBC_*` symbol the library
  imports must be at or below `GLIBC_2.28`, and no dynamic `libstdc++`
  may have leaked in;
- the wheel build, then `tests/test_python.py` and
  `tests/test_bridgestan_embed.py` against the installed wheel in a clean
  venv;
- [`tools/bs_conformance.py`](tools/bs_conformance.py) against reference
  BridgeStan on four fixtures. This checks stanli's interpretation of the
  public `bs_` interface against an implementation built from the reference
  source rather than only against stanli's own interface tests;
- the R package's tests under `R CMD check` against the library that
  build produced, with a missing-runtime skip treated as a failure;
- `clang-format` over tracked project-owned C/C++ files, excluding vendored
  dependencies and third-party code. The formatter version is pinned because
  output can differ across major versions.

Pull requests whose changes are limited to allowlisted Markdown/license files
or `web/index.html` instead run the generated-document checks and JavaScript
syntax checks. The required `manylinux_2_28_x86_64` gate remains present; an
unknown path fails closed onto the full source-change path.

Source-changing pull requests run the Linux x86_64 wheel job and the Windows
compiler-only gate. The other three wheel platforms (macOS arm64, macOS
x86_64, and manylinux aarch64), the full Windows C++ matrix, the ASan job, the
WebAssembly build with its eight-schools sampling test under Node, and the
webR side-module load test run on every push to `main`, nightly, and on
release tags. The full
Windows job builds the runtime and CTest suite, packages `stanli.dll`,
`stanli-compile.exe`, and pristine `stanc.exe`, then runs the installed-wheel
Python tests through source compilation, errors, lowering, gradients, sampling,
and generated quantities. These full platform checks report issues after a
pull request has merged rather than blocking that merge.

The nightly conformance sweep and its ratchet, the signature watch, and the
manually run model census are monitored separately from pull-request gates.
A ratchet checks whether recorded coverage decreased; the coverage totals
still need to be read separately.

Release tags additionally assert that the version in
`python/stanli/__init__.py`, `js/package.json` and `r/R/install.R` agrees
with the tag, that six runtime tarballs exist, and that the PyPI long
description renders.

Documentation consistency is also checked. Every
headline number in `README.md`, `python/README.md` and the demo page is
stamped from `docs/verification.json`, `docs/corpus-bench.tsv`, and the
representative-model choices in `docs/benchmarks.md` by
[`tools/gen_docs.py`](tools/gen_docs.py). Its `--check` mode fails CI when a
stamped number disagrees with those artifacts.

## Known limits

Two of the 120 posteriordb corpus models are not differentially verified.
`sir`'s ODE solution falls below a declared lower bound at every shared
evaluation point. CmdStan and stanli both reject those points,
so there is no numerical value to compare and the model is not counted as
verified. `kronecker_gp` matches log density to 1e-13 and 436 of 438
gradients, and differs by 0.7% on the two that flow through
`eigenvectors_sym` of a nearly degenerate covariance. Both are written up
in [`docs/corpus-status.md`](docs/corpus-status.md).

Transformed data has one engine. Every cross-path configuration shares
the MIR interpreter's answer there, so that section has no
stanli-internal cross-check and is covered only on the CmdStan axis.
A second independent implementation would be required for an internal
cross-path comparison of transformed data.

Reduction order is not frozen across releases. Fusion and packet
arithmetic change the order of some reductions, so gradients can differ
from an earlier release in their last bits. Releases that do this carry a
compatibility note in [`CHANGELOG.md`](CHANGELOG.md) with the worst
measured deviation against the untransformed graph. The environment switches
can then be used to identify the transformation responsible for a difference.

A small number of ULP relaxations are deliberate, each recorded at the
call site: the packet `log` path, and several reassociating Eigen
expressions at 1-2 ULP.

Some less-common multivariate and multinomial densities produce gradients
that match CmdStan but report `lp__` with a parameter-independent offset.
This leaves the mathematical normalized posterior and its gradient unchanged.
In finite-precision arithmetic, however, it can change a pinned-seed HMC
trajectory, and it matters when an application requires the absolute log
density. The implementation is described in
[`docs/compact-densities.md`](docs/compact-densities.md).

The optional `STANLI_LITE_LP` build makes the same trade across the whole
density library. It is off by default in every shipped build, including the
browser build, and `stanli_exact_lp()` reports which build is loaded. See
[`docs/lite-lp.md`](docs/lite-lp.md).

The conformance backlog is 287 `unexpected_unsupported` cases, each with
a reproduction command. These are generated cases that stanli has not yet
implemented; they are reported but do not fail the conformance run.

The cross-path ledger has four open entries, described above, one of
which records a known graph error.

The `STANLI_LITE_LP` build is not covered by CI, because covering it
would mean a second full stan-math compile. It is checked by hand with
`tools/verify_lite.py`.

## Review and maintenance practices

- Confirm the commit SHA associated with a merge or CI result. A successful
  run applies only to the commit it tested.
- Evaluate all three fixed points. Success at one point does not imply that a
  different branch reached at another point will compile or evaluate.
- Generate reported measurements from their source artifacts. Binary-size and
  coverage tables are generated, and `gen_docs.py --check` reports a mismatch
  between stamped values and those artifacts.
- Evaluate transformation tests twice in the same process. This checks for
  state retained from a previous evaluation.
- For a new safety guard, temporarily disable the guard and confirm that its
  regression test fails for the expected reason.
- Review changes to CmdStan references and coverage baselines with the code
  change that requires them. Reference updates record new evidence; they are
  not used only to remove a failing comparison.

## Reproducing this locally

Replaying the checked-in corpus references does not invoke CmdStan. The setup
script also prepares CmdStan because the recording and sampler tools use it:

```sh
./tools/dev_setup.sh --corpus     # native build, posteriordb, and CmdStan
python3 tools/verify_refs.py deps/posteriordb \
  --check build/stanli_check --jobs 8
ctest --test-dir build --parallel "$(tools/build_jobs.sh)"
```

The A/B and cross-path commands below use release-build tools that are not
built by the setup script's default release target list:

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

The WebAssembly replay additionally requires Node, Emscripten, and the stancjs
dependencies used by CI. After those prerequisites are installed:

```sh
./tools/build_web.sh
python3 tools/verify_refs.py deps/posteriordb \
  --check tools/wasm_check.sh --skip nn_rbm1bJ100
```

`./tools/dev_setup.sh --all` prepares the native corpus and conformance
dependencies; it does not install Emscripten or build WebAssembly.
[`docs/hacking.md`](docs/hacking.md) has the per-change recipes.
[`docs/how-it-works.md`](docs/how-it-works.md) describes the design the
numbers above are checking.
