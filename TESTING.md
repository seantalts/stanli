# Why should I trust this?

stanli reimplements the Stan runtime. This page lists the checks behind that
claim: what each one compares, against what, when it runs, and what it cannot
see.

There are three kinds of check. stanli is compared with CmdStan on the same
model, data and fixed points in unconstrained space: log density, every
gradient component and every value a draw writes. stanli's execution paths
are compared with each other bitwise, which catches an error one path would
hide. The samplers and other algorithms are compared with Stan's own service
code, because a gradient at a point cannot see a sampler setting. Agreement
at fixed points does not prove agreement over the whole parameter space.

## Numerical policy

This is the one place the acceptance rules live. Other pages link here.

A ULP is one step between adjacent doubles at a given magnitude. stanli and
CmdStan call the same stan-math with the same floating-point flags
(`-ffp-contract=off`, set project-wide in [`CMakeLists.txt`](CMakeLists.txt)),
so most differences come from evaluation order: `(a+b)+c` can differ from
`a+(b+c)` in the last bit.

A change may move a result away from CmdStan by:

- 2 ULP by default.
- 10 ULP for a reassociation kernel change (reductions, matvec, gather
  backward, softmax backward), when the commit message states that budget and
  the baseline moves in the same commit.
- 30 ULP for a density merged across loop lanes, where stanli sums in one call
  what CmdStan sums once per iteration. [`tools/corpus.py`](tools/corpus.py)
  lists the models that use this budget with the measured value and its cause.
- More, when a high-precision reference shows stanli at least as close to the
  true value as CmdStan. The reference and its provenance are recorded with
  the model. A kernel that replaces a nested-tape backward with a closed form
  is always held to such a reference:
  [`tools/matrix_pullback_hp_check.py`](tools/matrix_pullback_hp_check.py)
  evaluates the `matrix_exp` and matrix-solve adjoints at 60 digits, and the
  error relative to each matrix's largest entry must not get worse.

Anything beyond these budgets is a bug. Bitwise agreement with CmdStan is
reported for information and is never a gate. A change that trades a bitwise
match for a small ULP band and a speedup is accepted.

CI enforces:

- Bitwise equality in unit tests and between stanli's own execution paths. A
  kernel change that widens one of these records the new limit at the
  assertion.
- 10 ULP per element for the brms fixtures, against CmdStan references
  recorded on the same platform (`ULP_LIMITS` in
  [`tools/verify_refs.py`](tools/verify_refs.py)). For the models in
  `GRADIENT_AGAINST_LARGEST`, the gradient is measured in ULP of its largest
  entry.
- A scaled error `|a-b| / max(|a|, |b|, 1)` below 1e-9 for every other
  reference comparison. This gate catches implementation errors and does not
  prove a ULP bound; per-model ULP distances are in
  [`docs/corpus-status.md`](docs/corpus-status.md).
- Limits from measured deviations for the ill-conditioned models
  (`ILL_CONDITIONED`) and `kronecker_gp`.
- Matching rejection: a point CmdStan rejects must be rejected, and a
  non-finite value must match in kind.
- 10 ULP by default in the generated conformance sweep, with reviewed
  per-case exceptions.
- A separate gate for [fast mode](#fast-mode).

Kernels keep the reference summation order where practical, which is why the
unit tests are stricter than the corpus gate: reordering a matrix-vector sum
stays inside 1e-9 and still fails
[`tests/test_matvec.cpp`](tests/test_matvec.cpp) by 1 ULP. A kernel with an
allowance documents it at the call site, as the elementwise `log` and `exp`
kernels do in [`runtime/kernels/eltwise_expr.cpp`](runtime/kernels/eltwise_expr.cpp).

<a id="checks-run-before-and-after-merge"></a>

## The checks

"PR" means a pull request that changes source; one that touches only
documentation runs the static checks alone. A PR builds and tests on Linux
x86-64 (Clang) and Windows x64. "Post-merge" means pushes to `main`, nightly
runs, release tags and manual dispatch, and adds macOS arm64 and x86-64,
Linux aarch64 and a GCC build on Linux x86-64;
`gh workflow run wheels.yml --ref BRANCH` runs it on a branch. The jobs are in [`wheels.yml`](.github/workflows/wheels.yml),
[`lint.yml`](.github/workflows/lint.yml),
[`stan-conformance-nightly.yml`](.github/workflows/stan-conformance-nightly.yml),
[`r.yml`](.github/workflows/r.yml) and
[`rethinking.yml`](.github/workflows/rethinking.yml).

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [kernel unit tests](#unit-tests-for-numerical-operations) | one operation's value and every gradient component | the stan-math `var` call CmdStan's generated C++ would make, bitwise | PR (CTest) | whether lowering selects that kernel |
| [fused-density differential](#unit-tests-for-numerical-operations) | the seven fused densities over every activity mask and shape | Stan Math's kernels, bitwise | PR, Linux x86-64 | other platforms |
| [corpus replay](#comparison-with-cmdstan-on-complete-models) | log density, gradient and `write_array` rows of 361 models at 3 points | recorded CmdStan values | PR on Linux x86-64; post-merge on macOS arm64 and x86-64 and Linux aarch64 | parameter values away from the 3 points |
| [fast-mode replay](#fast-mode) | the same models compiled in fast mode | the same references, fast-mode gate | PR, Linux x86-64 | macOS, Windows and ARM |
| [sampling smoke](#comparison-with-cmdstan-on-complete-models) | 100 saved draws of the 13 Aalto fixtures | reference output names, draw count, finiteness | PR (CTest) | convergence |
| [cross-path matrix](#comparing-stanli-execution-paths) | every fixture under each execution configuration | each other, bitwise | PR (CTest) | transformed data, which has one engine |
| [pass A/B](#testing-graph-transformations) | log density and gradient with graph passes on and off | each other, within 1e-11 | by hand after pass changes | an error present in both cells |
| [vectorization A/B](#testing-graph-transformations) | stanc3's `vectorize_loops` pass off and on | each other, CmdStan references, gradient time | post-merge | a runtime change that affects both cells |
| [sampler parity](#samplers-and-other-algorithms) | NUTS draws and statistics across metrics and options | Stan's own NUTS services on the same executor, bitwise | PR (CTest) | compiler numerics (both sides share the executor) |
| [algorithm checks](#samplers-and-other-algorithms) | optimize, Laplace, Pathfinder, ADVI, fixed_param, RNG streams | a known Gaussian posterior; Stan's generator | PR (CTest) | non-Gaussian posteriors |
| [sampler trace](#samplers-and-other-algorithms) | step counts, tree depth, step size, divergences, mean `lp__` | CmdStan with the same settings | by hand after sampler changes | statistical validity of NUTS |
| [compiler producer parity](#compiler-producer-parity) | compact MIR bytes from the native, JavaScript and Windows compilers | each other, byte for byte | PR | numerical behavior |
| MIR wire cost | compact MIR decode time and size on Eight Schools | legacy MIR; each must be at most half | post-merge | other models |
| [generated conformance sweep](#testing-stan-language-coverage) | 24,322 generated signature and construct cases | reference BridgeStan from the same pins | nightly | operator-only behavior; cases the generator cannot build |
| [coverage ratchet](#testing-stan-language-coverage) | each generated case's status | the checked-in baseline | nightly | the coverage totals themselves |
| [integrated function models](#testing-stan-language-coverage) | 10 models using 436 function names, all outputs | recorded CmdStan values, 1e-9 | PR (CTest) | every overload of each name |
| [signature replay](#testing-stan-language-coverage) | 74 generated models covering 19,641 signatures, all outputs | recorded CmdStan values, 1e-9 | PR (CTest) | signatures the generators exclude |
| [stanc3 test models](#testing-stan-language-coverage) | the 1,244 models in stanc3's integration tests, lp, gradient and write_array at 5 points | recorded CmdStan values and messages, 1e-9; a status ledger | PR (CTest) | models whose generated data CmdStan rejects |
| Rethinking regeneration | the 62 Rethinking fixtures, regenerated from the book's code with pinned `rethinking` | the checked-in files, byte for byte | nightly | |
| [installed clients](#installed-clients) | the built wheel, R package and BridgeStan transport | API tests and R ecosystem consumers; skips fail | PR on Linux and Windows x64; post-merge adds wheel tests on macOS and Linux aarch64 and R tests on macOS arm64 | macOS and aarch64 before merge |
| [brms end to end](#samplers-and-other-algorithms) | `brm()` fits of the 124 brms formulas with the `stanli` backend | the same fits with brms's `cmdstanr` backend | by hand | anything beyond posterior means; platforms other than macOS arm64 |
| BridgeStan C ABI | 4 fixture models: values, names, counts, shapes | reference BridgeStan | post-merge | other models |
| downstream `stanr` | `stanr` built against this runtime, its backend tests | its own expectations | release tags, on demand | other R consumers |
| no-stdio build | undefined symbols of the runtime objects (via [`tools/check_no_stdio.sh`](tools/check_no_stdio.sh)) | the stdout, `printf`, `abort` and `exit` symbols `R CMD check` rejects | post-merge | |
| AddressSanitizer | the full CTest suite | no diagnostics | post-merge except release tags | a stale `vari` that points into reused arena memory |
| ThreadSanitizer | `test_capi`, `test_pool`, `test_multichain`, `test_wa_parallel` and `test_wa_parallel_abi` | no diagnostics | post-merge except release tags | the other tests |
| WebAssembly replay | log density and gradient of the corpus through the wasm build | the same references and gates | by hand | `write_array`; `nn_rbm1bJ100`, whose compile does not fit in 4 GB |
| Safari 17 bundle check | the js_of_ocaml bundles | no line ending in `static` | PR | |
| docs and formatting | stamped numbers, C/C++ formatting | their artifacts ([`tools/gen_docs.py --check`](tools/gen_docs.py)), the pinned formatter | every PR | |

Release tags also check the version strings, the runtime tarballs and the
PyPI description. Performance is never a correctness gate; the
[benchmark](docs/benchmarks.md#how-we-measure) keeps a timing pair only when
both engines agree within 1e-9 scaled error, and
[benchmarking a change](docs/benchmarks.md#benchmarking-a-change) covers
measuring one PR.

<a id="installed-clients"></a>

The R jobs run [`tools/test_r_ecosystem.R`](tools/test_r_ecosystem.R) against
the installed package. Its ecosystem testthat files (bayesplot, loo, posterior,
stanfit) must pass with no skip, failure, error or warning, so a test that
needs a package these jobs do not install, such as brms, goes in another file.

## Comparison with CmdStan on complete models

[`tools/verify_refs.py`](tools/verify_refs.py) replays every model in the
[corpus inventory](tools/corpus_inventory.py) against CmdStan's recorded
answers. It is the broadest whole-model comparison here and has found errors
the kernel tests did not reach.

[`corpus-refs.json.gz`](docs/internal/artifacts/corpus-refs.json.gz) holds
361 models at 3 points each: the posteriordb models that evaluate, language
fixtures adapted from stanc3, brms, Rethinking, Aalto, cogmod, ctsem and
OncoBayes2. Nearly all also carry `write_array` rows, compared by column name,
and both drivers seed Stan's RNG the same way so generated quantities compare
too. Per-model status and worst deviations are in
[`docs/corpus-status.md`](docs/corpus-status.md).

[`tools/verify_sample.py`](tools/verify_sample.py) records the references
through CmdStan's own driver ([`tools/ref_driver.cpp`](tools/ref_driver.cpp))
at the pins in [`deps/pins.env`](deps/pins.env), with the sampling log density
(`propto=true`) and the Jacobian. Points CmdStan rejects are recorded too,
because different points reach different branches. Linux x86-64 has its own
recording for the brms fixtures and a GCC supplement, because a CmdStan
gradient can differ by hundreds of ULP between Clang and GCC. The replay picks
the reference from the platform and compiler before evaluating; it never
picks whichever answer is closest. Missing points, changed inputs or
mismatched pins fail the run, and the tools that compile against CmdStan
refuse a checkout that does not match the pins
([`tools/toolchain.py`](tools/toolchain.py)).

A model in `KNOWN_GAPS` is one stanli refuses today. Its references are
recorded and the entry suppresses only the failure; when the gap closes, the
replay stays red until the entry is deleted. The list is currently empty.

[`tools/check_corpus_sampling.py`](tools/check_corpus_sampling.py) runs 100
warmup iterations and 100 draws of each fixture with `sampling_smoke`
metadata (the Aalto models), which must be finite, complete and named as in
the references.

## Fast mode

Fast mode (`fast_math`, see [docs/fast-mode.md](docs/fast-mode.md)) may reorder
sums and replace per-observation arithmetic with closed forms, so the ULP
budgets above do not apply. `tools/verify_refs.py --fast-math` replays
the same models and points against the same CmdStan references with every
model compiled in fast mode. At each point:

- The log density must be within 1e-12 scaled error.
- The largest absolute gradient difference, divided by the largest absolute
  gradient entry (or 1 if larger), must be below 1e-12. That is about 4,500
  ULP of the largest entry. The gradient is scaled as a whole because a
  coordinate that cancels to nearly zero can be thousands of ULP from
  CmdStan's while the gradient is accurate to its last few bits.
- Rejections and non-finite values must match, and `write_array` names and
  values keep the default 1e-9 gate.

`kronecker_gp` and the `ILL_CONDITIONED` models keep their default limits. A
point may otherwise exceed 1e-12 only when a high-precision reference shows
fast mode at least as close to the true value as CmdStan. The gate has not
been run on macOS, Windows or ARM.

[`tests/test_collapse.cpp`](tests/test_collapse.cpp) runs every graph the
observation collapse rewrites against the graph it came from, over every
argument form and activity variant, and compares the grouped forms with
extended-precision sums on data shifted by up to 1e9. These run only where
`long double` is wider than `double`. Measured accuracy is in
[docs/fast-mode.md](docs/fast-mode.md#how-accurate).

## Unit tests for numerical operations

A kernel is the runtime implementation of one operation. Each CTest builds the
call CmdStan's generated C++ would make with `stan::math::var` and compares the
value and every gradient component with `expect_eq`, which is bitwise. The
binaries are listed as `STANLI_TESTS` in [`CMakeLists.txt`](CMakeLists.txt);
`test_capi` links the shared library, so it tests the C ABI the packages use.

Two practices hold throughout. A regression test is first run against the
pre-fix source and must fail for the expected reason. Each test states the
autodiff activity of its reference arguments, because CmdStan's mixed
data/parameter instantiations can reassociate.

Two tests check properties rather than values.
[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) fills forward value
buffers with NaN between the sweeps, so any operation on the
`backward_ignores_values` allowlist that reads them fails. It also runs the
in-place, store-forwarding, rerolling, partitioning and island passes over 400
random graphs shaped like real models and compares gradients before and after.
`run_grad` in [`tests/graph_helpers.hpp`](tests/graph_helpers.hpp) evaluates
each call twice on one executor and fails on any bit difference, which catches
state leaking between evaluations.

Seven common densities run through fused kernels in
[`runtime/kernels/density_fused.cpp`](runtime/kernels/density_fused.cpp), and
shipped builds compile out the Stan Math path they replace. A build with
`-DSTANLI_STAN_DENSITY_ORACLE=ON` keeps both, and the `test_*_fused` tests
require them to agree bitwise over every activity mask, propto setting, shape,
edge value and rejection message.

Fixture MIR is generated from checked-in `.stan` files with the pinned
compiler (`./tools/gen_fixtures.sh`) and never checked in. Tests compile
through the same in-process pipeline users run; builds without it (Windows,
sanitizers) use the `stanli-compile` executable instead.

## Comparing stanli execution paths

Graph lowering, the MIR interpreter and the register program each map Stan
operations separately, so one can be wrong while another is right. Matrix
division was once implemented in the interpreter and missing from lowering.

[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles one model once per
configuration and compares results: islands off and forced
(`STANLI_NO_ISLAND`, `STANLI_ISLAND_ALWAYS`), generated adjoints off
(`STANLI_NO_NATIVE_ADJ`), the direct ODE path off (`STANLI_NO_ODE_DIRECT_RK`),
the graph passes off, and the interpreter running beside a complete
`write_array` graph (`STANLI_WA_FORCE_INTERP`). Stochastic `write_array`
columns, found by running the interpreter under two seeds, are excluded; the
CmdStan replay covers them with a matched seed.

The gate is bitwise for every row. A divergence fails unless
[`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) declares it
with a cause and a bound in ULP (-1 excuses any distance), so a new 1 ULP
difference is still reported. The ledger covers three operations only the
`write_array` graph supports, three region-map loops that group their sums
differently, and one open bug: one gradient entry each of
`higher_order_all_contexts` and `ode_adjoint_region`, bitwise on Linux and
macOS but far off in one Windows run, cause not yet found.

[`tests/test_cross_path.cpp`](tests/test_cross_path.cpp) drives the matrix
over every generated fixture and enforces minimum counts per path, so a
regression cannot hide as skips.

Focused tests hold the specialized paths to the same bitwise standard:
[`tests/test_adjoint.cpp`](tests/test_adjoint.cpp) for generated island
backwards, `test_island` for buffer reuse, and `test_ode_prog` and
`test_odevariadic` for the direct RK path against the Stan Math solve. A
platform disagreement narrows which models take the direct path; it never
loosens the gate.
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) lists the
switch for each of these paths.

## Testing graph transformations

[`harnesses/ab_corpus.py`](harnesses/ab_corpus.py) runs every posteriordb
model with selected graph passes off and on (by default rerolling, in-place
updates, constant folding and islands) and compares the log density and every
gradient component within 1e-11. It has caught pass errors in eight models.

<a id="mir-loop-vectorization-measurement"></a>
[`harnesses/vectorize_ab.py`](harnesses/vectorize_ab.py) does the same for
stanc3's `vectorize_loops` pass. Each model is compiled once with the pass off
and once on, and both cells must pass every semantic gate and the CmdStan
references. It also fails a model that is more than 1.04x slower with the pass on and
stays slower on a re-run.

## Compiler producer parity

The compiler runs as a native executable, as JavaScript and as a Windows
executable, and all three must emit the same compact MIR bytes.
[`tests/test_portable_stancjs.cjs`](tests/test_portable_stancjs.cjs) checks
this on the [compiler fixtures](tests/compiler/README.md), along with repeat
determinism, error and warning parity, and the stock `stanc()` API. The Python,
R and webR tests exercise includes through every compiler route.

## Testing Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py) reads every
signature the pinned compiler knows plus a catalog of language constructs,
generates a small type-correct model per applicable case, and evaluates it
with stanli and with a reference BridgeStan built from the same pinned CmdStan,
Stan, Math and stanc. It compares the log density and full gradient at fixed
points. The baseline
([`conformance-baseline.json.gz`](docs/internal/artifacts/conformance-baseline.json.gz))
holds 24,322 cases (24,291 signatures over 571 names, plus 31 constructs):
19,035 `verified`, 3,665 `inapplicable`, 923 `generator_gap`, 698
`expected_unsupported` and 1 `unexpected_unsupported`.
[docs/coverage.md](docs/coverage.md) explains the statuses and summarizes them
by function family; the [harness README](harnesses/conformance/README.md)
covers setup and local runs.

`mismatch`, `crashed` and `harness_error` fail the run; unsupported cases and
generator gaps are counted with reproduction commands. The ratchet fails when
a `verified` case stops verifying or the toolchain pin changes, and a
`signature-watch` job reports new upstream signatures.

The sweep cannot reach behavior used only through operators, cannot check a
case it cannot generate, and does not measure cost.
[Ten integrated function models](tests/function_coverage/README.md) close part
of that gap: 436 names, including data-only helpers, RNGs, solvers and five
operators, checked against recorded CmdStan values, gradients and outputs.

CTest also replays 74 generated models covering 19,641 signatures
([`tools/check_signature_models.py`](tools/check_signature_models.py)) against
recorded CmdStan values under the 1e-9 gate. Each case reads its own parameter
and writes its own output, so a failure names the signature. The manifests in
[`tests/function_coverage/`](tests/function_coverage/README.md) list what the
generators exclude.

stanc3's own integration tests cover syntax and type combinations that
posteriordb and the generators miss.
[`tools/check_stanc3_good.py`](tools/check_stanc3_good.py) runs all 1,244
models in `test/integration/good` and compares each one's log density,
gradient and `write_array` row with recorded CmdStan answers at five points
under the 1e-9 gate; where CmdStan throws, stanli must throw the same message.
[`tools/check_stanc3_bad.py`](tools/check_stanc3_bad.py) runs the 1,191 models
in `test/integration/bad` and compares the error text with stanc3's expected
output. Each has a ledger of the models that do not fully match, with a cause
([good](tests/stanc3_good/ledger.json), [bad](tests/stanc3_bad_ledger.json)),
and a model fails when it does better or worse than its entry says. See
[`tests/stanc3_good/README.md`](tests/stanc3_good/README.md).

## Samplers and other algorithms

Fixed-point tests cannot see sampler settings. When `run_nuts` once skipped
`set_max_depth`, trajectories were capped at 31 leapfrog steps instead of 1023
while every gradient comparison passed.

[`tests/test_sampler_parity.cpp`](tests/test_sampler_parity.cpp) runs the same
executor through stanli's NUTS driver and through Stan's own `hmc_nuts_*`
services, the ones CmdStan calls, and requires every saved row to be bitwise
equal. The cases cover unit, diagonal and dense metrics, adaptation on and off,
a supplied inverse metric and step size, thinning, saved warmup, and the RNG
that generated quantities draw between transitions.

[`tests/test_algorithms.cpp`](tests/test_algorithms.cpp) checks that each
algorithm draws from the random stream Stan's `create_rng` produces.
[`tests/test_capi_algorithms.cpp`](tests/test_capi_algorithms.cpp) runs
optimize, Laplace, Pathfinder, ADVI and fixed_param through the C ABI on a
correlated Gaussian: the mode must be within 1e-4, and Laplace, Pathfinder and
full-rank ADVI must recover the means and standard deviations within five
standard errors (wider for ADVI). `test_pathfinder`, `test_nuts` and
`test_walnuts` add algorithm-specific checks with fixed seeds.

[`tools/sampler_trace.py`](tools/sampler_trace.py) compares a full stanli run
with a CmdStan run of the same model, seed and lengths: total leapfrog steps,
maximum tree depth, final step size, divergence rate and scaled mean `lp__`.
Its tolerances catch large configuration errors, such as a 30-fold leapfrog
difference; keep its default adaptation target, since `--delta` reaches only
CmdStan.

Fast-mode posteriors are compared with default-mode and posteriordb reference
posteriors by [`harnesses/fast_sampling_check.py`](harnesses/fast_sampling_check.py),
run by hand; its flag rules are tested in CTest.

[`harnesses/brms_end_to_end.R`](harnesses/brms_end_to_end.R) fits each brms
formula through `brm()` and checks the common post-processing calls, and
[`harnesses/brms_compare_backends.R`](harnesses/brms_compare_backends.R)
compares two backends' posterior means in units of Monte Carlo standard error.
The [latest recording](output/brms-end-to-end/README.md) compares the `stanli`
and `cmdstanr` backends on macOS arm64.

## Known limits

The WebAssembly build is built in CI and loaded in webR, but its corpus
replay is run by hand. 119 of 120 posteriordb models compile and evaluate, and
118 of them are verified against CmdStan. The two that are not:

- `sir`'s ODE solution falls below a declared lower bound at every shared
  evaluation point. CmdStan rejects those points too, so there is nothing to
  compare. The brms model `s2_invgaussian` is also rejected by both engines at
  every shared point.
- `kronecker_gp` matches the log density to 1e-13 and 436 of 438 gradients.
  The two that flow through `eigenvectors_sym` differ by 0.7%, because the
  covariance's smallest eigenvalue gap is 6.5e-17 and eigenvector derivatives
  amplify last-bit input differences by about 1e16. Intel macOS has its own
  CmdStan recording for this model
  ([evidence](notes/performance/2026-09-22-intel-oracle.md)).

Transformed data has one engine. Every cross-path configuration shares the MIR
interpreter's answer there, so that block is checked against CmdStan only.

Reduction order is not frozen across releases. Fusion and packet arithmetic
can move gradients by their last bits within the budgets above; the release
notes give the worst measured deviation, and the switches in
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) identify the
transformation responsible.

Some less common multivariate and multinomial densities match CmdStan's
gradients but report `lp__` shifted by a parameter-independent constant. The
posterior is unchanged, but a pinned-seed trajectory can differ, and an
application that needs the absolute log density sees the shift. The optional
`STANLI_LITE_LP` build makes the same trade for every density; it is off in
shipped builds and checked by hand with `tools/verify_lite.py`. See
[docs/lp-constant.md](docs/lp-constant.md).

AddressSanitizer cannot see every lifetime error in stan-math's arena: a stale
`vari` can point into memory already reused by a live allocation. The
repeat-evaluation tests cover that case.

The nested callback solver fixtures allow 16 ULP on their combined rate
gradient, which must also match its analytic value within 2 ULP. The
difference comes from adjoint ODE sensitivity integration
([measurements](notes/execution/2026-09-28-nested-callback-coverage.md)).

## Reproducing locally

Replaying the checked-in references does not run CmdStan. The setup script
prepares CmdStan because the recording and sampler tools use it:

```sh
./tools/dev_setup.sh --corpus     # native build, posteriordb, and CmdStan
python3 tools/verify_refs.py deps/posteriordb \
  --check build/stanli_check --jobs 8
python3 tools/verify_refs.py deps/posteriordb \
  --check build/stanli_check --jobs 8 --fast-math
ctest --test-dir build --parallel "$(tools/build_jobs.sh)"
```

The A/B, cross-path, sampler and stanc3-model recording tools use release-build targets:

```sh
./tools/dev_setup.sh --conformance   # the reference stack for the sweep
cmake --build build-rel --parallel "$(tools/build_jobs.sh)" \
  --target stanli_check dump_ops stanli_shared
python3 harnesses/ab_corpus.py deps/posteriordb            # pass A/B
build-rel/stanli_check model.stan data.json --cross        # cross-path
python3 tools/check_corpus_sampling.py --build build-rel   # sampling smoke
python3 tools/sampler_trace.py \
  deps/cmdstan deps/posteriordb eight_schools_noncentered
python3 tools/check_stanc3_good.py --record     # stanc3 good references
cp build-rel/libstanli.* python/stanli/_bin/   # the sweep loads this copy
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python --filter log1p
```

The conformance harness needs Python 3.11 or later, or `tomli`; run it with
the `.venv-conformance` interpreter that `--conformance` creates.

AddressSanitizer is one CMake option:

```sh
asan_jobs=$(STANLI_JOB_MEMORY_GIB=12 tools/build_jobs.sh)
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTANLI_SANITIZE=address
cmake --build build-asan --parallel "$asan_jobs" && \
  ctest --test-dir build-asan --parallel "$asan_jobs"
```

The WebAssembly replay also needs Node, Emscripten and the stancjs
dependencies CI uses:

```sh
./tools/build_web.sh
python3 tools/verify_refs.py deps/posteriordb \
  --check tools/wasm_check.sh --target-platform 'WebAssembly wasm32' \
  --no-wa --skip nn_rbm1bJ100
```

[docs/hacking.md](docs/hacking.md) has the per-change recipes, and
[docs/how-it-works.md](docs/how-it-works.md) describes the design these checks
test.
