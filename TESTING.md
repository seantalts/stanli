# Why should I trust this?

stanli reimplements the Stan runtime. This page lists the checks that test
it: what each one compares, against what, when it runs, and what it cannot
see.

There are three kinds of check. The first compares stanli with CmdStan at
fixed points in unconstrained space: log density, every gradient component and
every value that a draw writes. The second compares stanli's execution paths
with each other bitwise. This catches an error that one path alone would
hide. The third compares the samplers and other algorithms with Stan's own
service code, because a gradient at a fixed point cannot show a wrong sampler
setting. Agreement at fixed points does not prove agreement over the whole
parameter space.

## Numerical policy

A ULP is one step between adjacent doubles at a given magnitude. stanli and
CmdStan call the same stan-math with the same floating-point flags
(`-ffp-contract=off`, set project-wide in [`CMakeLists.txt`](CMakeLists.txt)).
Most differences come from evaluation order: `(a+b)+c` can differ from
`a+(b+c)` in the last bit.

A change may move a result away from CmdStan by:

- 2 ULP by default.
- 10 ULP for a change to a reassociation kernel (reductions, matvec, gather
  backward, softmax backward). The commit message must state that budget, and
  the baseline must move in the same commit.
- 30 ULP for a density merged across loop lanes, where stanli sums in one call
  what CmdStan sums once per iteration. `NOTES` in
  [`tools/corpus.py`](tools/corpus.py) gives the cause for each such model.
- More, when a high-precision reference that is recorded with the model shows
  stanli at least as close to the true value as CmdStan. A closed-form
  replacement for a nested-tape backward always needs one. The `matrix_exp`
  and matrix-solve adjoints were evaluated at 60 digits.

Anything beyond these budgets is a bug. Bitwise agreement with CmdStan is
reported but does not fail a run. A speedup that gives up a bitwise match
within these budgets is accepted.

CI enforces:

- The limit written at each C++ unit test assertion, and bitwise equality
  between stanli's own execution paths except for divergences listed in the
  ledger.
- 10 ULP per element for the brms fixtures, against CmdStan references
  recorded on the same platform (`ULP_LIMITS` in
  [`tools/verify_refs.py`](tools/verify_refs.py)), and for the fixtures
  corpus on every platform. Some models measure the gradient in ULP of its
  largest entry: those in `GRADIENT_AGAINST_LARGEST` on every platform, and
  those in `GRADIENT_AGAINST_LARGEST_OFF_PLATFORM` on platforms other than the
  one that recorded them. Their covariance gradients cancel, and Eigen's
  matrix product fuses multiply-adds on arm64 and not on x86-64 or MSVC.
  CmdStan built on arm64, with Eigen's kernel changed to round each product,
  matches stanli on Linux within 2 ULP. Against an 80-digit reference, the
  largest gradient error is the same on both platforms.
- A scaled error `|a-b| / max(|a|, |b|, 1)` below 1e-9 for every other
  reference comparison. It does not prove a ULP bound; per-model ULP
  distances are in [`docs/corpus-status.md`](docs/corpus-status.md).
- Limits from measured deviations for the ill-conditioned models
  (`ILL_CONDITIONED`) and `kronecker_gp`.
- Matching rejection: stanli must reject a point that CmdStan rejects, and a
  non-finite value must match in kind.
- 10 ULP by default in the generated conformance sweep, with reviewed
  per-case exceptions.
- A separate limit for [fast mode](#fast-mode).

Kernels keep the reference summation order where practical, so the unit tests
are stricter than the corpus check. A reordered matrix-vector
sum stays inside 1e-9 but fails the zero-ULP cancellation case in
[`tests/test_matvec.cpp`](tests/test_matvec.cpp). A kernel with an allowance
documents it at the call site, as in
[`runtime/kernels/eltwise_expr.cpp`](runtime/kernels/eltwise_expr.cpp).

<a id="checks-run-before-and-after-merge"></a>

## The checks

"PR" means a pull request that changes source. A PR that touches only
documentation runs only the static checks. A PR builds and tests on Linux
x86-64 (Clang) and Windows x64. "Post-merge" means pushes to `main`, nightly
runs, release tags and manual dispatch. Post-merge runs add macOS arm64 and
x86-64, Linux aarch64 and a GCC build on Linux x86-64. To run them on a
branch, use `gh workflow run wheels.yml --ref BRANCH`. The jobs are in
[`.github/workflows`](.github/workflows): `wheels.yml`, `lint.yml`,
`r.yml`, `dev-setup-windows.yml`, `stan-conformance-nightly.yml`,
`rethinking.yml` and `stanr.yml`.

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [kernel unit tests](#unit-tests-for-numerical-operations) | one operation's value and every gradient component | the stan-math `var` call CmdStan's generated C++ would make, with the limit stated at each assertion | PR (CTest) | whether lowering selects that kernel |
| [lowering and pass tests](#unit-tests-for-numerical-operations) | log density, gradient, `write_array` rows and op counts of small compiled models | stan-math `var` references, hand-derived constants or the MIR interpreter | PR (CTest) | models outside the fixtures |
| [lit](#unit-tests-for-numerical-operations) | each marked `.stan` file's compile or evaluation result, and optionally one pass dump | the expectation written in the file | PR (CTest) | values the file does not state |
| [fused-density differential](#unit-tests-for-numerical-operations) | the seven fused densities over activity masks, shapes and edge values | Stan Math's kernels; bitwise, 2 ULP for normal and cauchy | Linux x86-64 on PRs and post-merge; AddressSanitizer; Windows ARM64 dev setup | macOS |
| [corpus replay](#comparison-with-cmdstan-on-complete-models) | log density, gradient and `write_array` rows of 373 models at 3 points | recorded CmdStan values | PR on Linux x86-64; post-merge on macOS arm64 and x86-64, Linux aarch64 and Linux GCC | parameter values away from the 3 points; Windows |
| [fast-mode replay](#fast-mode) | the same models compiled in fast mode | the same references, fast-mode limits | PR, Linux x86-64 | macOS, Windows and ARM |
| [local corpus replays](#comparison-with-cmdstan-on-complete-models) | log density, gradient and outputs of the function, signature, fixture and other local models | recorded CmdStan values | PR (CTest) | signatures and sizes the generators exclude |
| [stanc3 good models](#testing-stan-language-coverage) | the 1,382 models in stanc3's `good` integration tests at up to 5 points | recorded CmdStan values and messages, 1e-9; a status ledger | PR (CTest) | models whose generated data CmdStan rejects |
| [stanc3 bad models](#testing-stan-language-coverage) | compile errors of the 1,200 models in stanc3's `bad` integration tests | stanc3's expected output; a ledger | PR (CTest) | |
| [sampling smoke](#comparison-with-cmdstan-on-complete-models) | 100 saved draws of the 13 Aalto fixtures | reference output names, draw count, finiteness | PR (CTest) | convergence |
| [cross-path matrix](#comparing-stanli-execution-paths) | every fixture at 3 points and the 22 cogmod models at 13, under 18 execution configurations | each other, bitwise, with a ledger | PR (CTest) | transformed data, which has one engine; an error every path shares |
| [posteriordb cross-path](#comparing-stanli-execution-paths) | the posteriordb models under the same configurations plus one pass-off set | each other, within 1e-11 | by hand after pass changes | an error present in both cells |
| [sampler parity](#samplers-and-other-algorithms) | NUTS draws and statistics across metrics and options | Stan's own NUTS services on the same executor, bitwise | PR (CTest) | compiler numerics (both sides share the executor) |
| [algorithm checks](#samplers-and-other-algorithms) | optimize, Laplace, Pathfinder, ADVI, fixed_param, RNG streams | a known Gaussian posterior; Stan's generator | PR (CTest) | non-Gaussian posteriors |
| [sampler trace](#samplers-and-other-algorithms) | step counts, tree depth, step size, divergences, mean `lp__` | CmdStan with the same settings | by hand after sampler changes | statistical validity of NUTS |
| [compiler producer parity](#compiler-producer-parity) | compact MIR bytes from the native, JavaScript and Windows compilers | each other, byte for byte | PR | numerical behavior |
| MIR wire cost | compact MIR decode time and size on Eight Schools | legacy MIR; each must be at most half | post-merge | other models |
| [generated conformance sweep](#testing-stan-language-coverage) | 24,322 generated signature and construct cases, and each case's status | reference BridgeStan from the same pins; the checked-in baseline | nightly | operator-only behavior; cases the generator cannot build |
| Rethinking regeneration | the 62 Rethinking fixtures, regenerated from the book's code with pinned `rethinking` | the checked-in files, byte for byte | nightly; pushes to `main` that touch them | |
| [installed clients](#installed-clients) | the built wheel, R package and BridgeStan transport | API tests and R ecosystem consumers; skips fail | PR: wheel on Linux x86-64 and Windows x64, R on Linux. Post-merge: wheel on macOS and Linux aarch64, R ecosystem subset on macOS arm64 and Windows x64 | the R suite with a runtime on macOS and Windows |
| browser runtime | `tests/test_wasm.cjs` and `tests/test_worker_diagnostics.cjs`: compile, evaluate and sample Eight Schools through the browser build and its worker | expected values and API behavior | pushes to `main`; `test_wasm.cjs` again on release tags | pull requests and nightly runs |
| webR load | the WebAssembly side module | webR's loader and the C API symbols | post-merge | the R package flow |
| [brms end to end](#samplers-and-other-algorithms) | `brm()` fits of the 124 brms formulas with the `stanli` backend | the same fits with brms's `cmdstanr` backend | by hand | anything beyond posterior means; platforms other than macOS arm64 |
| BridgeStan C ABI | 4 fixture models: values, names, counts, shapes | reference BridgeStan | post-merge | other models |
| downstream `stanr` | `stanr` built against this runtime, its backend tests | its own expectations | release tags, on demand | other R consumers |
| no-stdio build | undefined symbols of the runtime objects (via [`tools/check_no_stdio.sh`](tools/check_no_stdio.sh)) | the stdout, `printf`, `abort` and `exit` symbols `R CMD check` rejects | post-merge | |
| Windows dev setup | [`tools/dev_setup.sh`](tools/dev_setup.sh) on Windows, then CTest | the full suite on ARM64 (through `stanli-compile`); six tests on x64 with the embedded compiler | pushes to `main`, on demand | the rest of the suite with the embedded compiler on x64 |
| AddressSanitizer | the CTest suite, compiling through `stanli-compile` | no diagnostics | post-merge except release tags | a stale `vari` that points into reused arena memory |
| ThreadSanitizer | `test_capi`, `test_pool`, `test_multichain`, `test_wa_parallel` and `test_wa_parallel_abi` | no diagnostics | post-merge except release tags | the other tests |
| WebAssembly replay | log density and gradient of the corpus through the wasm build | the same references and limits | by hand | `write_array`; `nn_rbm1bJ100`, whose compile does not fit in 4 GB |
| Safari 17 bundle check | the js_of_ocaml bundles | no line ending in `static` | PR | |
| docs and formatting | stamped numbers, C/C++ formatting | their artifacts ([`tools/gen_docs.py --check`](tools/gen_docs.py)), the pinned formatter | every PR | |

Release tags also check the version strings, the runtime tarballs and the PyPI
description. Three checks fail on time and compare no numbers: the MIR wire
cost step, `test_prep_scaling` (preparation must grow less than 20-fold when a
model grows 8-fold) and `test_prep_deep_expression` (a 20,000-deep expression
must prepare in under 3 seconds).
[Benchmarks](docs/benchmarks.md#how-we-measure) keep a timing pair only when
both engines agree within 1e-9 scaled error.

<a id="installed-clients"></a>

The wheel jobs run `tests/test_python.py` and `tests/test_bridgestan_embed.py`
on the installed wheel. They then install scipy and run the four Laplace
whitening and annotation tests (`test_whiten.py`, `test_laplace_whiten.py`,
`test_sparse_laplace.py`, `test_annotations.py`). Each of these skips with a
message when scipy is missing. The Linux R job runs `R CMD check` with the
runtime and then [`tools/test_r_ecosystem.R`](tools/test_r_ecosystem.R). The
macOS and Windows R jobs run only `test_r_ecosystem.R`. Its testthat files
(bayesplot, loo, posterior, stanfit) must pass with no skip, failure, error or
warning. Put a test that needs a package these jobs lack, such as brms, in
another file. The `r.yml` matrix runs `R CMD check` without a runtime, so its
sampling tests skip.

## Comparison with CmdStan on complete models

[`tools/verify_refs.py`](tools/verify_refs.py) replays every corpus against
CmdStan's recorded answers.
[`corpus-refs.json.gz`](docs/internal/artifacts/corpus-refs.json.gz) holds the
main [corpus](tools/corpus_inventory.py): 373 models at 3 points each. They are
119 posteriordb, 124 brms, 62 Rethinking, 22 cogmod, 13 Aalto, 11 regression
and 3 ctsem models, and 19 models from 9 CRAN packages (OncoBayes2,
rstanarm, multinma, EpiNow2, mlumr, rbioacc, BeeGUTS, hdbayes and
EcoEnsemble). Nearly all also carry `write_array` rows, which the replay
compares by column name. Both drivers seed Stan's RNG the same way, so
generated quantities compare too. Per-model status and worst deviations are in
[`docs/corpus-status.md`](docs/corpus-status.md).

`verify_refs.py --corpus NAME` replays a local corpus, and CTest runs each one:

| corpus | models | CTest |
| --- | --- | --- |
| `functions` | [10 models calling 436 names](tests/function_coverage/README.md), including data-only helpers, RNGs, solvers and five operators | `test_function_models` |
| `signatures` | 74 generated models covering 19,641 signatures, one output per case | `signature_refs_*` |
| `sized_functions` | 5 models calling library functions at real sizes | `test_sized_functions_reference` |
| `container_flow` | 66 models that write each container shape in each block and read it back | `test_container_flow_reference` |
| `fixtures` | 59 models from `tests/fixtures`, held to 10 ULP | `*_reference` |
| `cmdstan_replay` | 14 models for lowering rewrites that stock stanc's `--O1` MIR reaches | `test_cmdstan_replay` |
| `oncobayes2` | the 3 OncoBayes2 models | `test_oncobayes2_reference` |

`container_flow`, `cmdstan_replay` and `oncobayes2` also run through stock
stanc (the `_stanc` CTests). A model fails if it runs interpreted when its
corpus does not list it as interpreted, or the reverse. The main corpus replay
applies the same rule with `CORPUS_INTERPRETED` in
[`tools/verify_refs.py`](tools/verify_refs.py), which is empty.

[`tools/verify_sample.py`](tools/verify_sample.py) records the references, or
a local corpus with `--corpus NAME`. It uses CmdStan's own driver
([`tools/ref_driver.cpp`](tools/ref_driver.cpp)) at the pins in
[`deps/pins.env`](deps/pins.env), with `propto=true` and the Jacobian. It also
records points that CmdStan rejects. Linux x86-64 has its own brms recording
and a GCC supplement, because a CmdStan gradient can differ by hundreds of ULP
between Clang and GCC. The replay picks the reference by platform and compiler
before evaluating, never by closeness. The run fails on missing points,
changed inputs, and references recorded against an unpinned CmdStan, Stan or
Stan Math. [`tools/toolchain.py`](tools/toolchain.py) makes the recording
tools stop with an error on an unpinned CmdStan checkout. A model that stanli
refuses does not fail the run if it is listed in `KNOWN_GAPS` (empty today).
Once stanli supports the model, the run fails until you delete the entry.

[`tools/check_corpus_sampling.py`](tools/check_corpus_sampling.py) runs the
sampling smoke test, with 100 warmup iterations before the saved draws.

## Fast mode

Fast mode (`fast_math`, see [docs/fast-mode.md](docs/fast-mode.md)) may reorder
sums and replace per-observation arithmetic with closed forms, so the ULP
budgets above do not apply. `tools/verify_refs.py --fast-math` compiles every
model in fast mode and replays the same points against the same CmdStan
references. At each point:

- The log density must be within 1e-12 scaled error.
- The largest absolute gradient difference, divided by the largest absolute
  gradient entry (or 1 if larger), must be below 1e-12. That is about 4,500
  ULP of the largest entry. The gradient is scaled as a whole because a
  coordinate that cancels to nearly zero can be thousands of ULP from
  CmdStan's.
- Rejections and non-finite values must match. `write_array` names and values
  keep the default 1e-9 limit.

`kronecker_gp` and the `ILL_CONDITIONED` models keep their default limits.
Other points may exceed 1e-12 when a high-precision reference shows fast mode
about as close to the true value as CmdStan. `FAST_HP_CHECKED` lists those
points with their limit.

[`tests/test_collapse.cpp`](tests/test_collapse.cpp) compares every graph that
the observation collapse rewrites with the graph it came from, over every
argument form and activity variant. It also compares the grouped forms with
extended-precision sums on data shifted by up to 1e9. These tests run only
where `long double` is wider than `double`. Measured accuracy is in
[docs/fast-mode.md](docs/fast-mode.md#how-accurate).

## Unit tests for numerical operations

A kernel is the runtime implementation of one operation. Each kernel test
builds the call that CmdStan's generated C++ would make with
`stan::math::var`, and compares the value and every gradient component.
`STANLI_TESTS` in [`CMakeLists.txt`](CMakeLists.txt) lists the binaries.
`test_capi` links the shared library, so it tests the C ABI that the packages
use. A regression test must first fail against the pre-fix source, for the
expected reason.

[`tests/expect.hpp`](tests/expect.hpp) provides the assertions. `expect_eq` is
bitwise, so `+0` and `-0` differ and a NaN never matches.
`expect_eq_or_both_nan` also accepts two NaNs. `expect_ulp` and `expect_ulp_of`
take a ULP budget at each call site. Most kernel comparisons are bitwise.
Kernel budgets go up to 16 ULP, for the matrix-solve adjoints. Lowering tests
compare compiled models with the same references, hand-derived constants or
the MIR interpreter. Their budgets go up to 40 ULP where a chain of matrix
pullbacks reassociates. A few structured-loop, island and matrix-pullback
tests use a relative error of 1e-12 or 2e-12.

[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) fills forward value
buffers with NaN between the sweeps. The test fails if an operation on the
`backward_ignores_values` allowlist reads them. It also compares gradients
before and after the passes on 400 random graphs shaped like real models.
`run_grad` in [`tests/graph_helpers.hpp`](tests/graph_helpers.hpp) evaluates
each call twice on one executor and fails on any bit difference.

Seven common densities run through fused kernels in
[`runtime/kernels/density_fused.cpp`](runtime/kernels/density_fused.cpp).
Shipped builds compile out the Stan Math path that these kernels replace. The
CMake default `-DSTANLI_STAN_DENSITY_ORACLE=ON` keeps both paths, and
[`tests/test_density_fused.cpp`](tests/test_density_fused.cpp) compares them
over every activity mask, propto setting, shape, edge value and rejection
message. Normal and cauchy may differ by 2 ULP, and the others must match
bitwise. `ordered_logistic` runs three activity variants.

Each [lit test](tests/lit/README.md) is a marked `.stan` file. It states the
result that `stanli_check` must give and can match one pass dump. A known gap
is marked `XFAIL` and labeled `broken`.

## Comparing stanli execution paths

Graph lowering, the MIR interpreter and the register program each map Stan
operations separately, so one can be wrong while another is right.

[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles one model once per
configuration and compares results at three points on one executor. `configs()`
in that file lists the switches that each configuration sets. They cover
islands, island register compaction, generated adjoints, the direct ODE path,
the graph passes, data preloading, the region map's plans and the choices
inside a retained loop. The default configuration also runs the interpreter
beside a complete `write_array` graph and compares the rows. It leaves out
stochastic columns, which the CmdStan replay covers.

The check is bitwise. A divergence fails unless
[`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) lists it with a
cause and a bound in ULP (-1 allows any distance). The ledger lists three
operations that only the `write_array` graph supports, and summation
regroupings of 1 to 10 ULP by the region map and the lane passes. It also
lists one open bug: one gradient entry each of `higher_order_all_contexts` and
`ode_adjoint_region` was far off in one Windows run, and the cause is not yet
known.

[`tests/test_cross_path.cpp`](tests/test_cross_path.cpp) runs the matrix over
every fixture. When a fixture has no data file, the test generates unit data
from the declared inputs. It skips a fixture that the default configuration
cannot run. It requires at least 430 fixtures to run, 65 of them on generated
data. It also requires a minimum count per engine and a minimum number of
fixtures that each configuration changes, so a renamed switch fails. CTest
also runs each cogmod model through `stanli_check --cross --cross-points 13`,
where a skip fails.

Focused tests cover the specialized paths: `test_adjoint` for generated island
backwards, `test_island` for buffer reuse, `test_ode_prog` for the compiled ODE
right-hand side against the MIR interpreter, and `test_odevariadic` for the
direct RK path against Stan Math's solvers.

<a id="mir-loop-vectorization-measurement"></a>
Each graph pass also has unit tests (`test_reroll`, `test_inplace`,
`test_partition`, `test_cse` and others). After a pass change, run
`stanli_check --cross --cross-corpus --cross-rel-tol 1e-11` over posteriordb.
`--cross-corpus` adds `stanli_check`'s default point, and a configuration with
rerolling, in-place updates, constant folding and islands all off.
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) gives the loop,
its known failures and the switch for each path.

## Compiler producer parity

The native, JavaScript and Windows compilers must emit the same compact MIR
bytes.
[`tests/test_portable_stancjs.cjs`](tests/test_portable_stancjs.cjs) checks
this on the [compiler fixtures](tests/compiler/README.md). It also checks that
repeated runs give the same output, and that the JavaScript result and the
CLI's `--messages-json` file agree on errors, warnings and the O1 fallback
notice.

## Testing Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py) reads every
signature that the pinned compiler knows, plus a catalog of language
constructs. For each applicable case it generates a small type-correct model.
It evaluates that model with stanli and with a reference BridgeStan built from
the same pinned CmdStan, Stan, Math and stanc, and compares the log density
and full gradient at fixed points. The baseline
([`conformance-baseline.json.gz`](docs/internal/artifacts/conformance-baseline.json.gz))
holds 24,322 cases (24,291 signatures over 571 names, plus 31 constructs):
19,035 `verified`, 3,665 `inapplicable`, 923 `generator_gap`, 698
`expected_unsupported` and 1 `unexpected_unsupported`.
[docs/coverage.md](docs/coverage.md) explains the statuses and summarizes them
by function family. The [harness README](harnesses/conformance/README.md)
covers setup and local runs.

`mismatch`, `crashed` and `harness_error` fail the run. Unsupported cases and
generator gaps are counted and listed with reproduction commands. The ratchet
fails when a `verified` case stops verifying or the toolchain pin changes. A
`signature-watch` job reports new upstream signatures.

The sweep cannot test behavior that is used only through operators, or a case
that it cannot generate, and it does not measure cost. The `functions` and
`signatures` corpora cover part of that gap. The manifests in
[`tests/function_coverage/`](tests/function_coverage/README.md) list what the
generators exclude.

stanc3's integration tests cover syntax and type combinations that posteriordb
and the generators miss. [`tools/check_stanc3_good.py`](tools/check_stanc3_good.py)
runs the `test/integration/good` models and compares log density, gradient and
`write_array` row with recorded CmdStan answers. Where CmdStan throws, stanli
must throw the same message.
[`tools/check_stanc3_bad.py`](tools/check_stanc3_bad.py) compares the error
text of the `test/integration/bad` models with stanc3's expected output. A
model fails if it does better or worse than its ledger entry
([good](tests/stanc3_good/ledger.json), [bad](tests/stanc3_bad_ledger.json)).
See [`tests/stanc3_good/README.md`](tests/stanc3_good/README.md).

## Samplers and other algorithms

Fixed-point tests cannot see sampler settings.

[`tests/test_sampler_parity.cpp`](tests/test_sampler_parity.cpp) runs the same
executor through stanli's NUTS driver and through Stan's own `hmc_nuts_*`
services, which CmdStan calls. Every saved row must be bitwise equal. The
cases cover unit, diagonal and dense metrics, adaptation on and off, a
supplied inverse metric and step size, thinning, saved warmup, chain ids, and
the RNG that generated quantities draw between transitions. `test_sampling`
checks the default maximum tree depth, which the parity test sets explicitly.
It also samples a compiled conjugate normal with transformed data, a
transformed parameter and generated quantities, and compares the draws with
the closed-form posterior, which finds wrong state that one gradient call
leaves for the next.

[`tests/test_algorithms.cpp`](tests/test_algorithms.cpp) checks that each
algorithm draws from the random stream that Stan's `create_rng` produces.
[`tests/test_capi_algorithms.cpp`](tests/test_capi_algorithms.cpp) runs
optimize, Laplace, Pathfinder, ADVI and fixed_param through the C ABI on a
correlated Gaussian. The mode must be within 1e-4. Laplace, Pathfinder and
full-rank ADVI must recover the means and standard deviations within five
standard errors (wider for ADVI), with the reported log density and the
explicit and origin inits. `test_nuts` covers initialization retries and
failure. `test_pathfinder` covers determinism, inits, the per-iterate observer
and Pareto k-hat. `test_capi` checks `stanli_summary_stats` and
`stanli_diagnose_text`. The Python and R tests check only that each binding
passes arguments, results and errors through.

[`tools/sampler_trace.py`](tools/sampler_trace.py) compares a stanli run with a
CmdStan run of the same model, seed and lengths. It compares total leapfrog
steps, maximum tree depth, final step size, divergence rate and scaled mean
`lp__`. Its tolerances find large configuration errors, such as a 30-fold
difference in leapfrog steps. Keep its default adaptation target, because
`--delta` changes only the CmdStan run.
[`harnesses/fast_sampling_check.py`](harnesses/fast_sampling_check.py)
compares fast-mode posteriors with default-mode and posteriordb reference
posteriors. You run it by hand. CTest tests its flag rules.

[`harnesses/brms_end_to_end.R`](harnesses/brms_end_to_end.R) fits each brms
formula and checks the common post-processing calls.
[`harnesses/brms_compare_backends.R`](harnesses/brms_compare_backends.R)
compares the posterior means from the `stanli` and `cmdstanr` backends, in
Monte Carlo standard errors
([latest recording](output/brms-end-to-end/README.md)).

## Known limits

CI builds the WebAssembly build and loads it in webR, but its corpus replay
runs only by hand. 119 of 120 posteriordb models compile and evaluate, and 118
of them are verified against CmdStan. The two that are not:

- `sir`'s ODE solution falls below a declared lower bound at every shared
  point, and CmdStan rejects those points too. Both engines also reject the
  brms model `s2_invgaussian` at every shared point.
- `kronecker_gp` matches the log density to 1e-13 and 436 of 438 gradients.
  The two gradients that flow through `eigenvectors_sym` differ by 0.7%. The
  covariance's smallest eigenvalue gap is 6.5e-17, and eigenvector derivatives
  amplify last-bit input differences by about 1e16. Intel macOS has its own
  CmdStan recording for this model
  ([evidence](notes/performance/2026-09-22-intel-oracle.md)).

No cross-path fixture uses the data-loop memo, and the matrix sets no minimum
for memo hits. [`tests/test_data_loop_memo.py`](tests/test_data_loop_memo.py)
is its only check.

The `cmdstan_replay` references carry no CmdStan revisions, so a pin change does
not fail that corpus. No test covers the R interrupt path.

Reduction order is not frozen across releases. Fusion and packet arithmetic can
move gradients by their last bits within the budgets above. The release notes
give the worst measured deviation.

Under `~`, every density drops the data-only terms that CmdStan drops. The
`propto_data_arguments` and `propto_multi_gp` fixtures check this; see
[docs/lp-constant.md](docs/lp-constant.md).

The nested callback solver fixtures allow 16 ULP on their combined rate
gradient, which must also match its analytic value within 2 ULP. The difference
comes from adjoint ODE sensitivity integration
([measurements](notes/execution/2026-09-28-nested-callback-coverage.md)).

## Developer switches

The runtime reads about 60 `STANLI_*` environment variables. They turn a pass
off, force an engine or print a diagnostic
([docs/hacking.md](docs/hacking.md#building)). The CMake option
`STANLI_DEBUG_SWITCHES` compiles them in. It defaults to OFF, which is what
the wheels, the R runtime and the browser builds ship.
`tools/build_wheel.sh` stops with an error if `build-rel/` is configured ON.
`tools/dev_setup.sh` and the sanitizer jobs configure ON.

A test that sets a switch is listed in `STANLI_DEBUG_SWITCH_TESTS` in
[`CMakeLists.txt`](CMakeLists.txt) and carries the CTest label
`debug_switches`. In an OFF build, CTest reports it as `Not Run (Disabled)`
and the run still passes. CI runs CTest that way in the shipped `build-rel/`.
The manylinux job's differential step builds ON and runs
`ctest -L debug_switches`. Locally:

```sh
cmake -B build -DSTANLI_DEBUG_SWITCHES=ON
cmake --build build --parallel "$(tools/build_jobs.sh)"
ctest --test-dir build -L debug_switches
```

A test that needs only pass dumps, an execution report or a write_array
thread policy sets the typed fields instead, and runs in every build. The
fields are `CompileOptions::dump_passes`, `dump_stages`, `execution_report`
and `parallel_write_array`; the last two are also in `stanli_model_opts`.
`test_wa_parallel --switches` and `test_pass_dump --switches` are the labeled
halves that check the environment switches themselves.

## Reproducing locally

Replaying the checked-in references does not run CmdStan.
[`tools/dev_setup.sh`](tools/dev_setup.sh) configures `build/` and `build-rel/`
with `-DSTANLI_EMBED_STANC=ON` and `-DSTANLI_DEBUG_SWITCHES=ON` (`build-rel/`
with the density oracle off), and runs CTest in `build/`. Without the embedded
compiler or `deps/stanc3/stanli-compile`, every test that compiles Stan source
fails.

```sh
./tools/dev_setup.sh --corpus     # native build, posteriordb, and CmdStan
python3 tools/verify_refs.py deps/posteriordb \
  --check build/stanli_check --jobs 8    # add --fast-math for fast mode
python3 tools/verify_refs.py --corpus functions --check build/stanli_check
ctest --test-dir build --parallel "$(tools/build_jobs.sh)"
```

The cross-path, sampler and recording tools use release-build targets:

```sh
./tools/dev_setup.sh --conformance   # sweep stack and .venv-conformance
cmake --build build-rel --parallel "$(tools/build_jobs.sh)" \
  --target stanli_check dump_ops stanli_shared
build-rel/stanli_check model.stan data.json --cross        # cross-path
python3 tools/check_corpus_sampling.py --build build-rel   # sampling smoke
python3 tools/sampler_trace.py deps/cmdstan deps/posteriordb eight_schools_noncentered
python3 tools/verify_sample.py deps/cmdstan --corpus functions  # re-record
python3 tools/check_stanc3_good.py --record     # stanc3 good references
cp build-rel/libstanli.* python/stanli/_bin/   # the sweep loads this copy
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python --filter log1p
```

AddressSanitizer builds compile Stan source through `stanli-compile`, which
`./tools/dev_setup.sh --no-embed` builds:

```sh
asan_jobs=$(STANLI_JOB_MEMORY_GIB=12 tools/build_jobs.sh)
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTANLI_SANITIZE=address \
  -DSTANLI_DEBUG_SWITCHES=ON \
  -DSTANLI_PORTABLE_COMPILER="$PWD/deps/stanc3/stanli-compile"
cmake --build build-asan --parallel "$asan_jobs" && \
  ctest --test-dir build-asan --parallel "$asan_jobs"
```

The WebAssembly replay also needs Node, Emscripten and stancjs's dependencies:

```sh
./tools/build_web.sh
python3 tools/verify_refs.py deps/posteriordb \
  --check tools/wasm_check.sh --target-platform 'WebAssembly wasm32' \
  --no-wa --skip nn_rbm1bJ100
```

[docs/hacking.md](docs/hacking.md) has the steps for each kind of change, and
[docs/how-it-works.md](docs/how-it-works.md) describes the design that these
checks test.
