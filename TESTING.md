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

The project goal is agreement with CmdStan within 10 ULP most of the time,
with measured and explained exceptions. The checks below include tighter
local contracts and broader scaled-error gates; passing a scaled-error gate
does not establish a 10-ULP bound.

The numerical criterion depends on the comparison. Agreement with CmdStan is
measured in ULPs; the default policy is within 2 ULP. Bitwise agreement is not
a gate; a change that moves a model from bitwise to a small ULP band is
accepted. Reassociation-class kernel changes (reductions, matvec, gather
backward, softmax backward) may reach 10 ULP only when the change states that
budget in its commit message and updates the baseline in the same commit. A
density merged across loop lanes, where one call sums what CmdStan sums in
one call per iteration, may reach 30 ULP; the models that use the budget are
listed in `tools/corpus.py` with the measured value and its cause.
Anything beyond those budgets is a bug. Unit tests for individual operations and
the cross-path tests (stanli's own paths against each other) still use
bitwise equality; a kernel change that widens one of them records the new
limit at the assertion. Most corpus
points use a 1e-9 gate on scaled error `|a-b| / max(|a|, |b|, 1)`. Three
documented `kronecker_gp` points use limits based on measured deviations, and
points rejected by CmdStan require matching rejection behavior. The sections
below give the details and known exceptions.

A kernel that replaces a nested-tape backward with a closed form is held to a
higher-precision reference rather than to the tape.
[`tools/matrix_pullback_hp_check.py`](tools/matrix_pullback_hp_check.py)
evaluates the `matrix_exp` and matrix-solve adjoints at 60 decimal digits and
reports the largest error relative to each matrix's largest entry, old against
new. It also prints per-element ULP distance, but on entries near zero both
methods lose the same digits to cancellation, so the scale-relative error is
the measure a change must not worsen.

The CmdStan 2.40 upgrade also has focused fixtures in
`tests/fixtures/stan240_*.stan`, with independent three-point CmdStan references
in `tests/function_coverage/stan240_references.json.gz`. They cover the new
quantile and Poisson-binomial functions, container conversions, grouped
softmax, and normal-identity GLM design vectors across graph execution,
runtime control, transformed data and generated quantities. `test_stan240`
checks cross-path equality and direct Math values/gradients;
`test_stan240_reference` replays the external references. The generated
signature suite adds all supported overloads and mixed argument activity.

## Overview of the checks

The shared [corpus inventory](tools/corpus_inventory.py) supplies numerical
replay, sampling smoke tests and benchmark discovery. Source collections remain
provenance metadata and optional filters. Numerical replay covers all 329
referenced models; sampling smoke tests select fixtures through inventory
metadata, retaining complete output-name, draw-count and finiteness checks.
Live performance measurements use the [common benchmark protocol](docs/benchmark-protocol.md)
and are separate from CI correctness gates.

| check | question | acceptance rule | schedule |
| --- | --- | --- | --- |
| unit tests for numerical operations | Does one numerical operation or graph transformation agree with stan-math? | Bitwise by default; a recorded limit of at most 2 ULP (10 for reassociation) where a kernel reorders arithmetic | source-changing PRs |
| compiler producer parity | Do native OCaml, js_of_ocaml, and the Windows executable emit identical compact-v2 bytes while the stock rollback paths remain usable? | Byte-for-byte identity on fixture models, including the Stan 2.40 additions; JS API/error/warning/rollback checks; Windows provenance, executable-format, and final-newline checks | native/JS on source-changing PRs; Windows after merge and on demand |
| MIR wire cost | Is the compact-v2 decoder materially faster and the wire materially smaller than legacy MIR? | On Eight Schools, median decode time and raw bytes must each be at most half the legacy value | after merge, nightly, and on demand |
| corpus comparison | Are the 329 models in the shared corpus consistent with recorded CmdStan behavior at three fixed inputs? | Scaled error of 1e-9 for most points; documented limits for three `kronecker_gp` points and for every point of the three brms Gaussian-process models; rejection parity; a model named in `KNOWN_GAPS` must keep failing until its gap closes | source-changing PRs |
| corpus sampling smoke | Do inventory-selected source models produce complete saved draws? | Exactly 100 saved draws after 100 warmup iterations, exact reference output names/order, finite outputs and no missing columns | source-changing PRs, within CTest |
| cross-path matrix | Do stanli's execution paths agree with one another? | Bitwise, except entries named in the ledger | source-changing PRs, within CTest |
| transformation A/B | Do selected graph optimizations preserve model results? | Optimizations enabled and disabled agree at the default point within 1e-11 | manually after optimization changes |
| MIR vectorization A/B | What changes when the compiler's loop-vectorization pass is disabled? | Both modes pass numerical/reference checks; confirmed gradient-time regressions fail the run | after merge, nightly, and on demand |
| installed client checks | Do the built packages work through Python, BridgeStan's transport, and R? | Install the built artifacts; exercise APIs, sampling, outputs, the R compiler, and ecosystem consumers without missing-runtime skips | source-changing PRs |
| BridgeStan C-ABI comparison | Does the public C interface agree with reference BridgeStan? | Four fixture models must pass value, name, count, and output-shape checks | after merge, nightly, and on demand |
| downstream `stanr` embedding | Can the R package vendor this checkout and use Stanli through its `model_base` adapter? | `stanr` must build, then its Stanli-backend construction, derivative, sampling, data, output, init, and cache tests must pass | every version release and on demand |
| generated conformance sweep | For cases that both systems evaluate, do results agree, and which generated cases remain unsupported? | 10 ULP by default; reviewed per-case policy where needed | nightly and on demand |
| coverage baseline | Did a previously verified generated case stop verifying? | No loss of a verified case; obsolete policy exceptions must be removed | within the nightly sweep |
| integrated function models | Do ten mother-style models exercise supported function names and match CmdStan, including data-only and generated outputs? | Source/name coverage gate; log density, full gradients, names and every output at three points within scaled error 1e-9 | source-changing PRs, within CTest |
| model census | Do stanc3's 1,231 integration models still compile into stanli's runtime representation and, where checked, agree with CmdStan? | No decrease in per-model classification | manually, on demand |
| sampler trace | Is NUTS configured comparably to CmdStan? | Diagnostic summaries remain within limits chosen for large configuration errors | manually after sampler changes |
| AddressSanitizer | Does ASan detect invalid memory access while CTest runs? | No sanitizer diagnostics | after merge and nightly |
| WebAssembly replay | Does the browser runtime reproduce the recorded corpus? | Same numerical gates; 118 of 119 compiling posteriordb models fit in wasm32 | manually |
| browser compiler on Safari 17 | Do the js_of_ocaml bundles keep `static` on the same line as the class element it modifies? | No line in either bundle ends in `static` | source-changing PRs |
| documentation and formatting | Do generated claims match their artifacts, and is C/C++ formatting current? | Exact generated-file and formatter checks | every pull request |

## MIR loop-vectorization measurement

[`harnesses/vectorize_ab.py`](harnesses/vectorize_ab.py) measures one source
MIR pass against an explicit pass-off oracle: upstream `vectorize_loops`.
The test-only OCaml probe compiles each source once with the candidate off and
once with it on; every later check consumes those exact portable MIR files
through `stanli_check --mir`. All other source-pass choices are explicit and
identical between the two cells.

The complete run covers all 329 recorded models plus any posteriordb census
model without a recorded CmdStan row:

```sh
python3 harnesses/vectorize_ab.py deps/posteriordb \
  --compiler deps/stanc3/stanli-vectorize-probe \
  --check build-rel/stanli_check --bench build-rel/bench_grad \
  --dump build-rel/dump_ops --output-dir build-rel/vectorize-ab
```

The "MIR vectorization A/B" workflow step runs the complete corpus after
merge, nightly, on release tags, and on manual dispatch. PRs run the shipped
configuration once against all recorded CmdStan references, plus focused
compiler and runtime regression tests. They do not repeat the corpus through
the pass-off configuration or gate on timing ratios. The broader A/B sweep
remains useful for diagnosing a compiler pass and covering models without
CmdStan references; those are complementary post-submit checks.
The hard gates cover command/status consistency, result and write-array
categories, error parity, per-element finite/NaN/infinity classes, shapes and
names, and both pass modes against the existing CmdStan references. Finite
bit differences that remain within those gates are listed separately with
bit patterns and ULP distances. Preparation timings and, for a model whose
portable MIR differs between the two cells, op counts are diagnostics only:
the lowered log_prob graph growing, or the final log_prob graph growing by
more than 10% (with the runtime reroll pass on), are listed in `summary.md`
but never fail the run by themselves. The execution gate is gradient time on
the fixed `GRADIENT_MODELS` set: a model whose pass-on/pass-off ratio
exceeds 1.04 is re-measured with a fresh, independently interleaved run, and
the job fails only if that re-run also exceeds the ratio, so isolated
measurement noise does not fail the post-submit run.

The op-count diagnostic and the gradient gate only ever compare a run's own
pass-off cell against its pass-on cell, so a change that slows (or speeds
up) the runtime itself, identically in both cells, is invisible to them.
Passing `--baseline DIR` (a previous `--output-dir`) makes the harness also diff
final ops, lowered ops, island regions, and slots for every matching cell
against that directory's `graphs.jsonl`, and lists every model whose final
ops grew or shrank at the top of `summary.md`. This comparison is
report-only and never fails the run. On post-submit pushes to main, the
"Fetch the main-branch vectorization baseline" workflow step downloads the
`mir-vectorization-measurements` artifact from the most recent successful
run on main and passes it as `--baseline`; when no such artifact is found
(the first run, or one past its retention window) the step leaves the
baseline directory absent and the harness skips the comparison rather than
failing.

The output directory contains `manifest.json`, `corpus.jsonl`,
`graphs.jsonl`, `bench.tsv`, `summary.json`, and `summary.md`. The manifest
records source pins, producer provenance, tool hashes, platform, toolchain,
environment policy, and corpus scope. Compiler wall time, preparation
timings, executor arena slot counts, each measured process's peak resident
memory, and separate log-density/write-array reroll dispositions are
descriptive measurements only. Missing or malformed measurement output does
fail the run because it would make the report incomplete. The report labels
CmdStan-referenced and A/B-only models separately; the latter have off/on
category, error, shape, name, and value parity but no fabricated reference
gate. [`tools/corpus.py`](tools/corpus.py) remains the source-only census
report.

## Comparison with CmdStan on complete models

[`tools/verify_refs.py`](tools/verify_refs.py) replays every referenced
model in the shared [inventory](tools/corpus_inventory.py), including
posteriordb, compiler fixtures, brms, Rethinking and Aalto sources, against
CmdStan's recorded log density and full unconstrained gradient. This is the broadest whole-model numerical
comparison in the repository. It has found errors that isolated kernel tests
did not reach, which is why it runs in addition to the unit-test suite.

The reference artifact
[`docs/corpus-refs.json.gz`](docs/corpus-refs.json.gz) contains:

- 329 models at 3 deterministic unconstrained points each, 987 points in
  total, holding 358,925 log-density and gradient values. The models are
  119 posteriordb models that evaluate, 11 language fixtures adapted
  from stanc3's compiler tests, 124 models generated by brms 2.23.0,
  62 Rethinking fixtures (all 61 second-edition book calls plus a hurdle model),
  and 13 Aalto lesson fixtures.
- Values preserve the doubles emitted by CmdStan's driver
  ([`tools/ref_driver.cpp`](tools/ref_driver.cpp)). Original records use its
  exact `%.17g` strings; migrated records retain round-trippable JSON numbers
  and their original recording provenance. Migration does not rerun the oracle
  or change its numeric answers.
- 327 models carry at least one complete row from Stan's per-draw output
  routine, `write_array`, at the same points: 978 rows and 661,269 values
  covering constrained parameters, transformed parameters, and generated
  quantities. Column names are also compared exactly. Both direct
  `write_array` drivers start Stan's RNG with
  seed 1234 and chain 0, so generated-quantity draws are reproducible in
  this comparison. This is a controlled pointwise test; it does not claim
  to reproduce the RNG state after a CmdStan sampling run. For ordinary
  fixtures, the recorder stores an output row after stanli matches its names,
  width, and values. Strict sampling-smoke fixtures retain complete finite
  CmdStan outputs even if stanli fails, so recording cannot hide missing
  coverage. An absent row means that output is not covered. The
  per-model coverage is in [`docs/corpus-status.md`](docs/corpus-status.md);
  the aggregate above includes every source collection and all three points.
- Reference provenance recorded in the file: CmdStan
  2.40.0 at `d3d5df6a`, Stan `a6806ef8`, Math 5.4.0 at `5252d51d`, stanc3
  2.40.0 at `d58446e6`, posteriordb `28f8d3d6`, on Darwin arm64.

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

Of the 987 recorded points, 939 have status `VERIFIED`, 39 imported oracle
points have status `RECORDED`, six have status `REJECTED_BOTH`, and three have
status `MISMATCH`. Imported points use the same numerical replay gate; their
status preserves the distinction between recorded oracle answers and historical
cross-engine comparisons. The CmdStan 2.40
recording has no `CMDSTAN_ONLY` points. The three
`MISMATCH` points belong to `kronecker_gp`, where two eigenvector
gradients are sensitive to a nearly degenerate covariance whose smallest
eigenvalue gap is 6.5e-17. The formerly failing `s2_ar_cov` points and
all previously refused brms models, including `s2_com_poisson`, now
verify against the recorded values.

Verified points use the standard 1e-9 gate, except in the models named in
`ILL_CONDITIONED` ([`tools/verify_refs.py`](tools/verify_refs.py)). The
`MISMATCH` points use limits derived from their recorded deviations and
measured cross-platform variation. The three brms Gaussian-process models
`sw_gp`, `i320_gp_expquad` and `s2_gp_by_gr` retain their documented
cross-platform limits at every point. Their smallest Cholesky pivots are
1.1e-12, 3.7e-12 and 1.8e-12, respectively. All three now verify at every
point on the recording machine; `sw_gp` and `i320_gp_expquad` match
bitwise. Earlier references measured deviations of 1.03e-7 and 6.38e-9
at their third points on the x86_64 runner despite clean arm64 results.
Keeping these limits preserves the documented conditioning allowance
without disabling checks for other models.

A model in `KNOWN_GAPS` ([`tools/verify_refs.py`](tools/verify_refs.py))
is one stanli refuses today. Its references are recorded like any other
model's, and the entry suppresses only the failure. When the gap closes,
the model matches its references, the replay reports `GAP_CLOSED`, and
the run stays red until the entry is deleted.

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

### Sampling-smoke coverage

[`tools/check_corpus_sampling.py`](tools/check_corpus_sampling.py) uses the same
inventory and reference archive. Its default selection is the fixtures with
`sampling_smoke` metadata, currently all 13 imported Aalto models. Each source
model runs 100 warmup iterations and 100 saved draws. The checker requires
finite values, complete diagnostic columns, exact reference output names/order
and the expected draw count. Missing fixtures or reference output names fail.
This preserves coverage for transformed parameters and generated quantities.

```sh
python3 tools/check_corpus_sampling.py --build build-rel
ctest --test-dir build-rel -R 'test_corpus_sampling|test_run_timings' --output-on-failure
```

Use `--models NAME ...` to check other corpus fixtures with committed output
names. The short run tests execution and output completeness; convergence and
sampler statistics have their own tests. Performance uses the common benchmark
runner, with no collection-specific CI timing thresholds.

## How numerical agreement is measured

[`docs/corpus-status.md`](docs/corpus-status.md) publishes the worst
recorded deviations and replay coverage by model and source collection.
Where available, deviations include scaled error and ULPs. A ULP is
one step between adjacent representable floating-point numbers at a given
magnitude. Close agreement is expected because stanli and CmdStan both call
stan-math and use the same floating-point compiler flags. Evaluation order
still matters: floating-point addition is not associative, so `(a+b)+c` can
differ from `a+(b+c)` in the last bit.

The policy is: agreement within 2 ULP by default. Reassociation-class kernel
changes (reductions, matvec, gather backward, softmax backward) may use a 10
ULP budget when the change is measured and that budget is stated in the commit
message. Densities merged across loop lanes may use 30 ULP, recorded per
model. A larger distance from CmdStan is acceptable when a high-precision
reference shows stanli at least as close to the true value as CmdStan is;
the reference measurement and its provenance must be recorded with the model.
Bitwise agreement is reported for information but is not a gate; if a
change improves performance by moving a model from bitwise to a small ULP band,
that is an accepted trade. At their primary recorded point, 55 verified
posteriordb models have 0 ULP difference with CmdStan. Eight additional language
fixtures have 0 ULP difference in [`docs/verification.json`](docs/verification.json).

Kernels preserve the reference summation order where practical to stay within
the 2 ULP budget. The contract in [`docs/hacking.md`](docs/hacking.md) gives a
concrete example: reordering a matrix-vector sum remains within the corpus's
1e-9 scaled-error gate but changes the result by 1 ULP, which
[`tests/test_matvec.cpp`](tests/test_matvec.cpp) detects. Kernel paths with
an explicit ULP allowance are documented at their call sites. For example, the
elementwise `log` kernel uses Eigen's packet implementation, which can differ
from the system math library by 1 ULP on some inputs. The corresponding packet
`exp` path is not used because its accumulated effect moves `kronecker_gp`
outside its measured reference gate
([`runtime/kernels/eltwise_expr.cpp`](runtime/kernels/eltwise_expr.cpp),
line 333). Several other Eigen expressions have documented differences of 1-2
ULP because they reassociate arithmetic.

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
compiler. CMake creates these ignored build artifacts before any native target
that consumes them. To regenerate them explicitly, run:

```sh
./tools/gen_fixtures.sh
```

`tools/gen_fixtures.py` owns the O0/O1 policy and canonical spelling. The
`.hpp` files that stanc also writes beside the models are removed. Neither the
MIR nor the C++ output is checked in.

Core `tools/dev_setup.sh` builds both compiler artifacts from the configured
stanc3 source revision: the executable, which fixture generation and the
signature model generators run, and the in-process compiler object, which
`stanli_check` links. The source-level lit cases, the function model replay,
and the signature model replay all compile through that in-process pipeline,
the one `stanli_run` and the language packages ship, so they see the same
source passes a user does. A build without the object (the Windows and
AddressSanitizer CI jobs) runs the same pipeline through its executable,
`stanli-compile`, which CMake copies from `deps/stanc3/` to sit beside
`stanli_check`, the way the Windows wheel ships it beside the runtime.
`stanli_check --stanc PATH` runs an external stanc instead, for A/B work and
compiler bisects.

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
The Python and installed R interface tests also exercise filesystem includes:
automatic source-directory lookup, ordered `include_paths`, nested and repeated
includes, filenames with spaces and Unicode, missing files and cycles, and exact
gradient/draw agreement with equivalent single-file models. They verify that
changing the sampling seed still works after the include files are deleted.
R checks the real bundled JavaScript compiler through V8 and the webR file
transport; subprocess checks exercise the portable and stock compilers when
available. `test_capi` covers the embedded include entry point in native and
sanitizer builds.
The PR compiler job also runs `tests/test_webr.mjs --compiler-only` in real
webR, checking nested includes and missing-file diagnostics through both the
portable compiler and legacy fallback. The side-module load check stays
post-submit.
The portable compiler suite also checks repeat determinism, source-bearing
error objects, warning parity, the normal `stanc()` JavaScript API against
stock stancjs on ordinary inputs, and final-newline behavior. The overflow fixture is excluded from that
API comparison because the checked producer policy deliberately retains an
expression where pristine stanc3's host-width-dependent folds disagree. A
focused worker harness proves the preferred custom
import and the missing-artifact fallback import. The tested JavaScript
artifacts are the ones consumed by the Pages and npm jobs. The manylinux gate
then checks the typed-producer output with `test_mir_decode`,
produced by the probe with `vectorize_loops` off since `stanc --O1` has no
switch for it.
Post-submit measurements use `bench_mir_decode`. For Eight Schools, compact v2
must take no more than half the legacy decoder's
median time across 51 repetitions and no more than half its raw bytes. Gzip and
complete preparation timings remain descriptive measurements in the uploaded
artifact.

The same parity check covers the Windows producer after merge, nightly,
on release tags, and on demand. The
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
`STANLI_NO_NATIVE_ADJ`, `STANLI_NO_ODE_DIRECT_RK`,
rerolling/in-place updates/constant folding off, and the shipped pipeline with
`STANLI_WA_FORCE_INTERP` attaching `WaInterp` beside a complete `write_array`
graph so both engines produce the same output row. The ODE configuration
retains the generated direct-RK payload but selects the canonical Stan Math
solve, so the comparison isolates execution rather than preparation.

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

[`tests/test_cross_path.cpp`](tests/test_cross_path.cpp) drives the matrix over
every generated `tests/fixtures/*.tmir.sexp` file, so a new `.stan` fixture is
included automatically. Fixtures that cannot compile or evaluate are recorded
as skipped. Minimum counts are enforced for each execution path and output
mode, preventing a broad regression from appearing only as additional skips.
The exact current thresholds are kept beside the test.

Generated adjoints have a separate comparison. `gen_adjoint`
([`runtime/src/adjoint.cpp`](runtime/src/adjoint.cpp)) generates an
island's backward as a second instruction list at load time;
`STANLI_NO_NATIVE_ADJ=1` restores the replay under stan-math's `var`,
which is what [`tests/test_adjoint.cpp`](tests/test_adjoint.cpp) checks
the generated program against.

Replayed islands reuse executor-owned handle buffers only when a definite
initialization proof covers every register read and live-out on every path.
`STANLI_NO_REPLAY_REUSE=1` (set before executor creation) restores fresh buffers.
`STANLI_NO_DEAD_CONSTANTS=1` disables local removal of overwritten constant
initializers, and `STANLI_NO_FILL_SINK=1` disables delaying range initialization
past unused branches; set these before compiling the model. `test_island`
covers the proof's refusal paths, loops, indexed spans, nested evaluation,
exceptions, copied executors, workers, and the fresh-buffer comparison.

Native-adjoint islands can additionally select the shared three-lane softmax
forward specialization. `STANLI_NO_ISLAND_SOFTMAX3=1` leaves the admitted
island and its generated backward intact but uses the canonical forward
program, giving a focused A/B switch. `test_adjoint` compares the specialized
and canonical forward register files bit-for-bit, checks the generated
backward, and covers the activation threshold, clone budget, overlap, and NaN
behavior. `test_island` covers the opt-out through graph carving and execution,
tagged-payload lifetime, and the rule that opcode zero remains invalid for
graph carving and binding after the private helper is registered.

The direct RK path has two additional focused gates. `test_ode_prog` checks
that derivative construction leaves the canonical RHS bytecode unchanged,
requires bitwise local RHS values and `J_y`/`J_theta` entries for admitted
programs, and pins structural refusals including runtime jumps, `DOT`, and
signed-zero-sensitive `FMAX`/`FMIN`. `test_odevariadic` compares the default
path against a separately lowered `STANLI_NO_ODE_DIRECT_RK=1` oracle for RK45,
CKRK, and legacy RK45. It compares solution bits, the complete Jacobian
scratch matrix, both input pullbacks, whole-model log density and gradient,
all scalar-activity masks, shared-spec multithreaded execution, and exception
type and text. Data-only, BDF/Adams, ineligible RHS programs, and
`forward_value_only` remain on their established paths.

The direct/oracle gate is bitwise; it has no ledger tolerance. The integrated
macOS arm64 Release A/B admitted the default path with 1.8615x
[1.8380x, 1.8853x] oracle/direct on `lotka_volterra` and 1.8035x
[1.7578x, 1.8505x] on `soil_incubation`, while the ineligible branched BDF
control was unchanged. Linux x86-64 must run both default and
`STANLI_NO_ODE_DIRECT_RK=1`, the focused ODE tests, and the ODE interface sweep
before merge. Any architecture-specific disagreement narrows eligibility or
keeps the oracle; it does not relax the exact gate. This testing contract is
for the all-double coupled-sensitivity path. The slower Phase 0
precomputed-gradient callback bridge remains stopped and is not selected by
either arm.

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
meaningful gradient case under the generator's current rules; it does not
mean the function has no testable values or effects. The definitions are in
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

PR #299 exposed a concrete gap in this distinction. In the checked-in
baseline, the real-valued `reverse`, `block`, `to_matrix`, and container
`rep_array` cases were generator gaps. The spacing, fill, and CSR-index
helpers were generator gaps or inapplicable because they require data-only
inputs or return integers. `.^` was outside the function-call inventory.
None of those classifications established support on the graph, transformed
data, or generated-quantity paths. The focused fixtures now run in CTest, with
Stan Math value/gradient checks and cross-path comparisons. In addition,
[ten integrated function models](tests/function_coverage/README.md) cover 430
names, including data-only helpers, RNGs, solvers and five operators, against
recorded CmdStan values, gradients and complete outputs on every PR. They
cover supported forms per name rather than every overload. Nonuniform matrix
weights are necessary: comparing only `sum(to_matrix(a))` cannot detect a
permutation of `a`. The generated inventory still needs value-only cases and
broader container/operator generation before it can cover these scopes
automatically; a green coverage gate is not a claim of complete support.

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

## Building without stdio

The R package stanr vendors this repository's runtime sources into a
CRAN-style package, and `R CMD check` rejects compiled code that references
stdout, stderr, `printf`, `puts`, `abort`, `exit`, or the assert failure
path. [`tools/check_no_stdio.sh`](tools/check_no_stdio.sh) configures the
runtime object library with `-DSTANLI_NO_STDIO`, builds it, and scans every
object file's undefined symbols for that list, failing with the offending
file and symbol if any appear. The `no-stdio` job in
[`.github/workflows/wheels.yml`](.github/workflows/wheels.yml) runs it on
main pushes, nightly runs, release tags, and manual dispatches.

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

PRs use one representative native build and one complete external model
oracle. Focused tests cover individual kernels, compiler contracts, execution
paths, and clients; broad alternative-configuration sweeps and platform
matrices run post-submit. The source-change path is defined in
[`.github/workflows/wheels.yml`](.github/workflows/wheels.yml) and
[`.github/workflows/lint.yml`](.github/workflows/lint.yml):

- Static checks: generated documentation, development-setup contracts,
  browser syntax, Windows exports, CI routing, and pinned C/C++ formatting.
- One Linux x86_64 Clang build: the full CTest suite, including focused
  cross-path and pass-safety tests, and every recorded CmdStan model at all
  three points. Numerical thresholds and the reference corpus are unchanged.
- Installed wheel checks: Python and BridgeStan transport tests in a clean
  environment, truthful manylinux tags, and the binary-size artifact.
- Shared compiler checks: native/JavaScript producer parity, typed/legacy
  MIR decoding, source/artifact provenance, and demo-model metadata.
- One Linux R integration job using that runtime: `R CMD check`, compilation
  through V8, and ecosystem acceptance with missing-runtime or acceptance
  skips treated as failures. The R workflow also retains its inexpensive
  ABI/provenance job on relevant PRs.

Pull requests whose changes are limited to allowlisted Markdown/license files
including `AGENTS.md`, research material under `notes/`, or `web/index.html`
instead run static checks and formatting. Executable code and test fixtures
must not live under the documentation-only `notes/` directory. The required
`manylinux_2_28_x86_64` status remains present for every PR. It checks static
validation and, for source changes, the native build, shared compiler, and R
integration. A failed, cancelled, or unexpectedly skipped prerequisite cannot
produce a green gate; unknown file paths select the full source-change path.

Main pushes, nightly runs, release tags, and manual dispatches additionally
run the full vectorization A/B corpus, portable-MIR cost measurements, live
reference BridgeStan comparisons, cross-release R compatibility, first-posterior
timings, no-stdio configuration, Windows compiler parity, the other native
platforms, WebAssembly, and webR. ASan and TSan run on the same non-PR events
except release tags. The standalone R platform/version matrix runs on relevant
main pushes, release tags, and manual dispatches. Rethinking regeneration runs
after relevant main pushes, nightly, and on demand.

The full Windows job builds the runtime and CTest suite, packages `stanli.dll`,
`stanli-compile.exe`, and pristine `stanc.exe`, then runs the installed-wheel
Python tests through source compilation, errors, lowering, gradients, sampling,
and generated quantities. These checks report failures for prompt follow-up
after merge and preserve the existing release dependencies. If a change needs
specific platform or optimization evidence before landing, run the relevant
focused check or dispatch the full workflow on its branch:

```sh
gh workflow run wheels.yml --ref my-change
```

The nightly conformance sweep and its ratchet, the signature watch, and the
manually run model census are monitored separately from pull-request gates.
A ratchet checks whether recorded coverage decreased; the coverage totals
still need to be read separately.

Release tags additionally assert that the version in
`python/stanli/__init__.py`, `js/package.json` and `r/R/install.R` agrees
with the tag, that six runtime tarballs exist, and that the PyPI long
description renders. Before any full-release publisher runs, the release
workflow also calls [`.github/workflows/stanr.yml`](.github/workflows/stanr.yml)
to rebuild the pinned downstream `stanr` package with the tagged runtime and
run its Stanli-backend construction, derivative, sampling, data, output,
initialization, and cache tests. The check remains available on demand through
manual workflow dispatch.

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
from an earlier release in their last bits, within the 2 ULP default budget
or the 10 ULP reassociation budget. Releases that do this carry a
compatibility note in [`CHANGELOG.md`](CHANGELOG.md) with the worst measured
deviation against the untransformed graph. The environment switches can then
be used to identify the transformation responsible for a difference.

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
