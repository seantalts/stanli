# stanli

The Stan Language Interpreter: an op-graph executor over precompiled
stan-math kernels. No C++ toolchain, no LLVM, no compilation on the
user's machine.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![npm](https://img.shields.io/npm/v/@seantalts/stanli.svg)](https://www.npmjs.com/package/@seantalts/stanli)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)
[![R](https://github.com/seantalts/stanli/actions/workflows/r.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/r.yml)
[![License](https://img.shields.io/pypi/l/stanli.svg)](LICENSE)

**Try it in your browser, no install:**
[seantalts.github.io/stanli](https://seantalts.github.io/stanli/) --
Stan source to posterior draws in a few hundred milliseconds, entirely
client side.

One runtime, three packages. Each is the same shared library behind a
different binding, so a model samples to the same draws from any of them.

| | | |
| --- | --- | --- |
| **Python** | `pip install stanli` | [python/README.md](python/README.md), [PyPI](https://pypi.org/project/stanli/) |
| **R** | `install.packages("stanli")` then `stanli_install()` | [r/README.md](r/README.md) |
| **Browser / Node** | `npm install @seantalts/stanli` | [js/README.md](js/README.md), [npm](https://www.npmjs.com/package/@seantalts/stanli) |

The R package is not on CRAN yet; until it is, install it from
[r-universe](https://seantalts.r-universe.dev) or from a checkout (see
[r/README.md](r/README.md)). It downloads its runtime on first use
rather than bundling it, because CRAN will not carry a 16 MB binary and
could not build one on their farm.

- Performance vs CmdStan: [docs/benchmarks.md](docs/benchmarks.md).
  Median gradient <!--gen:corpus_median-->2.07x<!--/gen--> across
  <!--gen:corpus_n_grad-->119<!--/gen--> posteriordb models,
  <!--gen:corpus_at_par-->100<!--/gen--> of them at or above CmdStan;
  <!--gen:bench_span-->1.0x-6.1x<!--/gen--> on vectorized shapes;
  time-to-first-draw about 20x faster.
- Model coverage: [docs/corpus-status.md](docs/corpus-status.md).
  <!--gen:corpus_verified-->118/120<!--/gen--> posteriordb models
  verified against CmdStan's log density and full gradient,
  <!--gen:corpus_bitwise-->45<!--/gen--> of them bitwise identical,
  worst relative deviation <!--gen:corpus_worst-->2.6e-12<!--/gen-->.
- Distribution coverage: [docs/coverage.md](docs/coverage.md). 71 of 72
  densities, 90 of 105 cdf/lcdf/lccdf, truncation, censoring, ordinal
  regression and the count GLMs, each bitwise against CmdStan.
- Install size: one 22.2 MB shared library, a 7.8 MB wheel. Breakdown in
  [Binary size](#binary-size).
- How this is possible, for statisticians:
  [docs/how-it-works.md](docs/how-it-works.md)
- Tutorial, three small models traced through every layer:
  [docs/lowering-walkthrough.md](docs/lowering-walkthrough.md)
- Contributor map: [docs/hacking.md](docs/hacking.md)
- Design doc: `docs/superpowers/specs/2026-08-04-stan-portable-runtime-design.md`

## Architecture

The premise: a Stan model does not need machine code generated for it.
Every model is a composition of a fixed vocabulary of operations
(densities, constraint transforms, linear algebra, elementwise math).
stanli ships those operations precompiled and turns each model into
data: a static graph of ops over flat buffers, built at load time and
run by a small interpreter. There is no JIT and no C++ codegen;
"compiling" a model takes milliseconds.

```
model.stan + data.json
  |  stanc3 (official OCaml compiler, linked into the library)
  v
transformed MIR (s-expression)
  |  lowering: runtime/src/lower.cpp
  v
op graph + preallocated value/adjoint arenas
  |  executor: forward = log density, reverse = gradient
  v
NUTS (stan::mcmc::adapt_diag_e_nuts) -> draws
```

1. **stanc3, in process.** The official Stan compiler (OCaml) is built
   as a self-contained object and linked into the shared library. It
   parses, typechecks, and optimizes the model; stanli consumes its
   transformed MIR. Full language fidelity without a subprocess or a
   reimplemented parser.

2. **Lowering** (`runtime/src/lower.cpp`). Transformed data is evaluated
   eagerly, loops with data-known bounds are unrolled, and the model
   block flattens into a linear op sequence over preallocated arenas.
   `~` statements lower to the same propto and per-argument-activity
   instantiations CmdStan's generated C++ uses, so dropped constants
   match exactly. The one user function that cannot be inlined is an ODE
   right-hand side (the integrator picks the times), and it compiles
   into a flat register machine (`runtime/src/ode_prog.cpp`) instead.

3. **Graph passes** (`runtime/src/reroll.cpp` and friends; plain-language
   guide in [runtime/src/OPTIMIZATIONS.md](runtime/src/OPTIMIZATIONS.md)).
   A model written as a per-observation loop arrives as N copies of one
   small op template, and the interpreter's cost is per op. The passes
   rewrite those regions into the vectorized ops the kernels already
   support: constants become vectors, invariant ops hoist, indexed reads
   become gathers, and N scalar density terms fuse into one summed
   vector density. `radon_pooled` goes from 27,670 ops to 8. Anything a
   pass cannot prove safe it leaves alone. `STANLI_NO_REROLL=1` disables
   the main pass.

4. **Execution** (`runtime/src/executor.cpp`). The op graph is the AD
   tape. The forward sweep computes the log density and stashes each
   op's partials; the reverse sweep runs the ops backward, contracting
   adjoints. Steady-state gradient evaluation allocates no memory.

5. **Kernels** (`runtime/kernels/`). Two tiers behind one interface.
   Native kernels mirror the exact Eigen expressions of stan-math's rev
   overloads, so gradients match CmdStan bitwise (FP contraction pinned
   off project-wide). Everything else runs as a "legacy" op: a recorder
   scalar or a nested var tape drives unmodified stan-math templates.
   Legacy ops make the whole library expressible; native kernels make
   the hot path fast. Both compile once, when stanli is built.

6. **Sampling** (`runtime/src/nuts.cpp`). Stan's own NUTS with
   diagonal-metric adaptation, driven through a thin model adapter.

7. **Writing draws.** A second, forward-only graph lowers the MIR's
   `generate_quantities` section and produces every CSV column CmdStan
   would write, in CmdStan's order and under CmdStan's naming. Models
   the graph cannot express (RNG draws, branches on draw-computed
   values) run through a per-draw interpreter instead
   (`runtime/src/wa_interp.cpp`), so all 119 compiling corpus models
   produce their full columns.

8. **Distribution.** Everything sits behind a C ABI
   (`runtime/include/stanli/capi.h`) in one shared library. Each binding
   is a thin wrapper over it; a platform wheel is one .whl containing
   one dylib.

## Binary size

One self-contained shared library, 22.2 MB installed, 7.8 MB compressed
in the wheel. Attributed by demangled symbol:

| | | |
| --- | ---: | ---: |
| densities and distribution functions | 12.03 MB | 54.9% |
| embedded stanc3 (all OCaml) | 5.73 MB | 26.1% |
| Boost, nlohmann/json, NUTS, libc++, unattributed | 1.61 MB | 7.3% |
| stanli itself | 0.92 MB | 4.2% |
| stan-math, everything else | 0.78 MB | 3.6% |
| Eigen (out-of-line) | 0.71 MB | 3.3% |
| SUNDIALS | 0.14 MB | 0.7% |

The densities dominate. A distribution is instantiated once per activity
mask (which arguments are autodiff), twice for propto, and again for the
elementwise form: `4 * 2^N` templates per distribution, about 630 KB of
object each. That is the standing cost of shipping precompiled math;
CmdStan instantiates only the combination your model uses and pays for
it with a per-model compile. Each density chooses how much of that
ladder to instantiate (`STANLI_SCALAR_DENSITY_LIST` in optable.hpp): the
thirteen distributions models lean on take all of it, the long tail
takes less, and the 72 distribution functions take one instantiation
each.

`-DSTANLI_LITE_LP=ON` drops the propto family: the library is 48%
smaller and every gradient stays bitwise, but `lp__` differs from
CmdStan's by a per-model constant. It is **off by default in every
build, browser included**, so any run can be compared against CmdStan
directly; `stanli_exact_lp()` reports which build is loaded. See
[docs/lite-lp.md](docs/lite-lp.md).

The interpreter and NUTS together are about 410 KB. Shrinking further
was measured and declined: dead-code stripping cannot reach inside the
OCaml object, and compiling stanc3 to bytecode saves 3.7 MB at the cost
of about 8x slower model compilation.

### The browser build

A different binary: no embedded stanc3, because the compiler ships
separately as JavaScript (2.84 MB, 0.40 MB gzipped; a page that ships
precompiled MIR never fetches it). `stanli.wasm` is 5.79 MB raw and
1.52 MB gzipped, and densities are 55% of the compressed payload
(measured by stubbing every density kernel and relinking). Loading the
uncommon densities on demand was built and removed; the measurements and
the emscripten limitation that blocks it are in
[docs/density-pack.md](docs/density-pack.md).

## Python

A ctypes wrapper over the shared library, published to PyPI as one
platform wheel per platform. Full documentation in
[python/README.md](python/README.md).

```python
import stanli
m = stanli.Model(stan_file="model.stan", data={"J": 8, "y": y, "sigma": s})
fit = m.sample(seed=1, chains=4, warmup=1000, samples=1000)
fit["mu"].mean()          # every draw, chains concatenated
print(fit.summary())      # stansummary's table
print(fit.diagnose())     # the convergence checks, in words
```

Four chains by default, run in parallel. Threading does not change the
answer: each chain owns its executor and its RNG stream, so the draws
are byte-identical to a sequential run. Builds without the embedded
stanc3 object fall back to running a bundled stanc binary as a
subprocess.

## R

The same runtime behind an R binding, with `posterior`-shaped draws.
Full documentation in [r/README.md](r/README.md).

```r
library(stanli)
stanli_install()   # one time: fetches the runtime for this platform

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)
summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
```

The runtime is downloaded on first use because CRAN cannot build or
carry it; `stanli_install()` is explicit and pins the release the
package was built against. The Stan compiler is stanc3 compiled to
JavaScript, run through V8 (rstan's approach) when the runtime does not
embed it. Because binding and runtime are separately versioned, the C
ABI carries a layout version (`stanli_abi_version()`) and the bridge
refuses a runtime that disagrees; reading the options struct at wrong
offsets would not crash, it would sample from the wrong seed.

## Browser (WASM)

The same runtime compiles to WebAssembly and runs full Stan in a browser
tab with no server:
**[seantalts.github.io/stanli](https://seantalts.github.io/stanli/)**.
stanc3's js_of_ocaml build compiles the model to MIR in JS; stanli.wasm
lowers it and samples. Eight schools goes from source to 1,000 draws in
about 120 ms in-tab. 118 of the 119 compiling corpus models replay
against the CmdStan references from inside WASM (`tools/wasm_check.sh`;
the exception is `nn_rbm1bJ100`, whose compile does not fit in wasm32's
4 GB).

```
./tools/build_web.sh              # emsdk + opam builds, assembled in web/
python3 -m http.server -d web     # then open http://localhost:8000
```

## Build

One-shot setup (fetches pinned deps, builds, runs tests):

```
./tools/dev_setup.sh            # core build + tests
./tools/dev_setup.sh --embed    # + OCaml toolchain, in-process stanc3
./tools/dev_setup.sh --corpus   # + posteriordb and the CmdStan verify rig
./tools/dev_setup.sh --all
```

Or manually:

```
./deps/fetch.sh
cmake -B build
cmake --build build -j
ctest --test-dir build
```

## Releasing

`.github/workflows/wheels.yml` builds all five wheels (macOS arm64 and
x86_64, manylinux_2_28 x86_64 and aarch64, Windows x86_64). The first
four run on every push and pull request; Windows runs after the merge
and nightly, because mingw compiles the density kernels slowly enough to
set the pace of every merge. A release tag runs all five, and the
publish job waits for them. Each build links the cached embedded stanc3
object, runs the test suite, checks the platform tag, and samples eight
schools from the installed wheel in a clean venv.

To cut a release: bump `__version__` in `python/stanli/__init__.py` (the
one place it lives), add a `CHANGELOG.md` entry, then tag. The publish
job fires only on `refs/tags/v*`, asserts the tag matches the packaged
version, and uploads through PyPI trusted publishing; no API token
exists anywhere in the repo. The `pypi` deployment environment is
restricted to `v*` tags as a second lock.

```
git tag -a v0.1.0 -m "stanli 0.1.0" && git push origin v0.1.0
```

No sdist is published: building from source needs a 30-minute OCaml
toolchain step, so an sdist would only turn "no wheel for your platform"
into a confusing build failure.

The npm package `@seantalts/stanli` ships the same way on its own tag
series: bump `version` in `js/package.json`, add the changelog entry,
tag `npm-vX.Y.Z`. The `npm-publish` job asserts the tag matches
`package.json` and publishes through npm trusted publishing. Two npm
quirks worth knowing: a trusted publisher attaches only to a package
that already exists, so a package's first version goes out by hand with
`npm publish`; and the package is scoped because npm's name-similarity
filter rejects unscoped `stanli`, which is why `publishConfig.access` is
set to public.

### R

The same `v*` tag publishes the R side (`runtime-release` job). It
attaches to the GitHub Release five runtime tarballs
(`stanli-runtime-{darwin,linux,windows}-{arm64,x86_64}.tar.gz`, what
`stanli_install()` downloads) plus `stanli_X.Y.Z.tar.gz`, the R source
package. The job asserts `stanli_runtime_release` in `r/R/install.R`
equals the tag: the package pins its runtime release on purpose, so bump
the pin in the same commit as the version.

Bumping `STANLI_ABI_VERSION` in `runtime/include/stanli/capi.h` means
bumping `STANLI_R_ABI_VERSION` in `r/src/bridge.c` too; `r.yml` fails if
they disagree. Bump it when a C ABI struct changes layout or a function
changes signature; adding a function does not need one.

Two R distribution channels:

- **r-universe** builds from this repository continuously; every push to
  main becomes an installable binary with no tag. It needs a one-time
  registry entry in `seantalts/seantalts.r-universe.dev`.
- **CRAN** cannot be automated by policy: a submission is a web-form
  upload confirmed by email. `r.yml` runs `R CMD check --as-cran` on
  every change under `r/`; a CRAN release is then bump `Version:` in
  `r/DESCRIPTION` plus the runtime pin, tag, download the tarball from
  the release, and upload it at
  [cran.r-project.org/submit.html](https://cran.r-project.org/submit.html).

The R sampling tests run in `wheels.yml` against the Linux library that
build produced (and fail if skipped); `r.yml` has no runtime, which is
an honest simulation of CRAN's farm.

## Verification policy

Nothing ships on "looks close". Kernel gradients are bitwise-tested
against stan-math's var path at fixed points; whole models are
differentially verified against CmdStan at the same deterministic
evaluation point (`tools/verify_sample.py`). The corpus scoreboard
(`tools/corpus.py`) tracks which posteriordb models compile, evaluate,
and verify. Details in [docs/corpus-status.md](docs/corpus-status.md).

## Status

Built and tested in CI on macOS (arm64, x86_64) and manylinux (x86_64,
aarch64). 119/120 posteriordb models compile and evaluate,
<!--gen:corpus_verified_n-->118<!--/gen--> of them CmdStan-verified. Of
the two that are not: `sir`'s ODE solution dips about 1e-9 below a
declared lower bound at every shared evaluation point and CmdStan
rejects it there too, and `kronecker_gp` matches on lp and 436 of 438
gradients, differing on the two that flow through eigenvectors of a
nearly degenerate covariance (see the note in the corpus status).

## Roadmap

The path to displacing CmdStan rather than out-running it on a corpus is
written up in
[docs/superpowers/plans/2026-08-08-cmdstan-parity-roadmap.md](docs/superpowers/plans/2026-08-08-cmdstan-parity-roadmap.md):
multi-chain and diagnostics (done), the missing parameter transforms
(done), the modern `ode_*` and solver interfaces, Pathfinder and
optimize, a native adjoint program for the sequential tail (done;
[design](docs/superpowers/plans/2026-08-08-native-adjoint-program.md)),
`reduce_sum`, and the R/brms and browser packaging.

Engine-level items that predate it:

1. Fusing adjacent elementwise chains into one pass over the arena, now
   that the re-roll pass covers the mixture shape and tape islands
   compile the leftover scalar residue.
2. A CRAN shim package.
3. Vectorized kernels via stan-math's varmat (SoA) overloads. Today the
   kernels mirror CmdStan's default AoS arithmetic, which is scalar for
   transcendentals and reductions. The plan: switch kernels to mirror
   the varmat expressions function-by-function where the overload
   exists, verify differentially against `stanc --O1` CmdStan builds,
   and keep AoS parity for the rest. Profile a large-N model first to
   size the win.
