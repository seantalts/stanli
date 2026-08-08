# stanli

The Stan Language Interpreter: an op-graph executor over precompiled
stan-math kernels. No C++ toolchain, no LLVM, no compilation on the
user's machine.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)

**Try it in your browser, no install:**
[seantalts.github.io/stanli](https://seantalts.github.io/stanli/) --
Stan source to posterior draws in a few hundred milliseconds, entirely
client side.
[![License](https://img.shields.io/pypi/l/stanli.svg)](LICENSE)

```
pip install stanli
```

- Performance vs CmdStan: [docs/benchmarks.md](docs/benchmarks.md)
  (median gradient <!--gen:corpus_median-->2.07x<!--/gen--> across
  <!--gen:corpus_n_grad-->119<!--/gen--> posteriordb models,
  <!--gen:corpus_at_par-->93<!--/gen--> of them at or above CmdStan;
  <!--gen:bench_span-->1.0x-6.1x<!--/gen--> on the vectorized shapes that
  suit it best; time-to-first-draw ~20x faster)
- Model coverage: [docs/corpus-status.md](docs/corpus-status.md)
  (<!--gen:corpus_verified-->118/120<!--/gen--> posteriordb models
  differentially verified against CmdStan's log_prob and full gradient,
  <!--gen:corpus_bitwise-->45<!--/gen--> of them bitwise identical, worst
  relative deviation <!--gen:corpus_worst-->2.6e-12<!--/gen-->; per-model
  accuracy in relative terms and ULPs is listed there)
- Install size: one 21.3 MB shared library, a 7.4 MB wheel. Breakdown
  in [Binary size](#binary-size) below; the browser build halves the
  runtime with `STANLI_LITE_LP` ([docs/lite-lp.md](docs/lite-lp.md)).
- Distribution coverage: [docs/coverage.md](docs/coverage.md) (46 of 72
  densities, 87 of 105 cdf/lcdf/lccdf, truncation and censoring; every one
  bitwise against CmdStan, and what is missing says why)
- How this is possible, for statisticians:
  [docs/how-it-works.md](docs/how-it-works.md)
- Design doc: `docs/superpowers/specs/2026-08-04-stan-portable-runtime-design.md`

## Architecture

For a contributor's file-by-file map and the how-to-add-a-function
recipe, see [docs/hacking.md](docs/hacking.md).

The premise: a Stan model does not need machine code generated for it.
Every model is a composition of a fixed vocabulary of operations
(densities, constraint transforms, linear algebra, elementwise math), so
stanli ships those operations precompiled and turns each model into data:
a static graph of ops over flat buffers, built at load time and executed
by a small interpreter. There is no JIT and no C++ codegen; "compiling" a
model takes milliseconds.

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

Stage by stage:

1. **stanc3, in process.** The real Stan compiler (OCaml) is compiled to
   a self-contained object (`-output-complete-obj`) and linked into the
   shared library. It parses, typechecks, and optimizes the model, and
   stanli consumes its transformed MIR directly. Full language fidelity
   without a subprocess or a vendored parser rewrite.

2. **Lowering** (`runtime/src/lower.cpp`). A compile-time interpreter
   walks the MIR against the actual data: transformed data is evaluated
   eagerly, loops with data-known bounds are unrolled, and the model
   block flattens into a linear sequence of ops reading and writing
   preallocated arenas. `~` statements lower to the same propto +
   per-argument-activity instantiations CmdStan's generated C++ uses, so
   dropped constants and skipped data partials match exactly. The one
   user function that cannot be inlined is an ODE right-hand side --
   the integrator picks the times, so it has to stay callable at
   runtime -- and that compiles instead into a flat register machine
   (`runtime/src/ode_prog.cpp`), which is worth 29-39x on the models
   that use one.

3. **Graph passes** (`runtime/src/reroll.cpp` and friends; a
   plain-language guide to all of them is in
   [runtime/src/OPTIMIZATIONS.md](runtime/src/OPTIMIZATIONS.md)).
   Unrolling is what makes
   the graph concrete, but a model written as a per-observation loop
   arrives as N copies of one small op template, and the interpreter's
   cost is per op. This pass finds those periodic regions and rewrites
   them into the vectorized ops the kernels already support: constant
   vectors materialized from the const pool, invariant ops hoisted,
   elementwise lanes widened, index progressions collapsed into their
   base vector, and N scalar density terms fused into one summed vector
   density. `radon_pooled` goes from 27,670 ops to 8. Whatever the pass
   cannot prove safe it leaves alone, one region at a time.
   `STANLI_NO_REROLL=1` disables it.

4. **Execution** (`runtime/src/executor.cpp`). The op graph is the AD
   tape. The forward sweep computes the log density and stashes each
   op's partials in per-op scratch; the reverse sweep runs the ops
   backward, contracting adjoints. Steady-state gradient evaluation
   performs zero allocation, which is where the speedup over the
   pointer-chasing var tape comes from.

5. **Kernels** (`runtime/kernels/`). Two tiers behind one interface.
   Native kernels are hand-written forward/backward pairs that mirror
   the exact Eigen expressions of stan-math's rev overloads, so
   gradients match CmdStan bitwise (FP contraction pinned off
   project-wide). Everything else runs as a "legacy" op: a recorder
   scalar (`rvar`, a registered stan-math scalar type) or a nested var
   tape replay drives unmodified stan-math prim/prob templates and
   deposits values and partials into the caller's buffers. Legacy ops
   make the whole library expressible; native kernels make the hot path
   fast. Both are compiled once, when the stanli binary is built.

6. **Sampling** (`runtime/src/nuts.cpp`). Stan's own NUTS with
   diagonal-metric adaptation, driven through a thin model adapter that
   returns one precomputed-gradients vari per evaluation.

7. **Writing draws.** The log_prob graph deliberately does not compute
   transformed parameters the target never reads, and never sees
   generated quantities at all, so a second graph does: the MIR's
   `generate_quantities` section lowers, through the same machinery and
   the same passes, into a forward-only graph that takes an unconstrained
   draw and produces every CSV column CmdStan would write, in CmdStan's
   order and under CmdStan's naming (`theta.1.1` for array elements,
   column-major for matrices). 93 of the 119 compiling corpus models get
   theirs in full; the rest stop at an RNG call or at generated
   quantities that branch on a parameter, and emit the prefix that did
   lower along with the reason they stopped
   (`harnesses/wa_coverage.py`, `harnesses/wa_header_check.py`).

8. **Distribution.** Everything above sits behind a C ABI
   (`runtime/include/stanli/capi.h`) in one shared library; the Python
   package is a ctypes wrapper around it. A platform wheel is one .whl
   containing one dylib.

## Binary size

One self-contained shared library, 21.3 MB installed, 7.4 MB compressed
in the wheel. Attributing its 21.0 MB of code and data by symbol:

| | | |
| --- | ---: | ---: |
| densities and distribution functions | 11.43 MB | 54.4% |
| embedded stanc3 (all OCaml) | 5.73 MB | 27.3% |
| stan-math, everything else | 0.64 MB | 3.1% |
| Boost, nlohmann/json, NUTS, libc++, unattributed | 1.53 MB | 7.3% |
| stanli itself | 0.84 MB | 4.0% |
| Eigen (out-of-line) | 0.68 MB | 3.2% |
| SUNDIALS | 0.14 MB | 0.7% |

The densities are the majority, and the split above is measured the same
way as the rest of the table: by demangled symbol, with everything
matching a `_lpdf`, `_lpmf`, `cdf`, `lcdf`, `lccdf` or `_rng` name
counted against the first row rather than against stan-math generally.
A distribution is instantiated once per activity mask -- which arguments
are autodiff -- twice for propto and again for the elementwise form, so
4 * 2^N templates per distribution, about 630 KB of object each. That is
the standing cost of shipping precompiled math: CmdStan instantiates
only the combination your model uses, and pays for it with a per-model
compile.

Each density picks how much of that ladder to instantiate
(`STANLI_SCALAR_DENSITY_LIST` in optable.hpp). The thirteen distributions
models lean on take the whole thing; the long tail takes less. The 72
distribution functions (`cdf`/`lcdf`/`lccdf`, which is what truncation
runs on) take the least -- one instantiation each -- and that choice was
measured: giving the common ones the mask dispatch cost 4.8 MB on its
own, against 2.2 MB for the whole family without it. The 34 scalar math
functions cost 0.03 MB between them, because an elementwise kernel with a
hand-written derivative instantiates nothing.

**`-DSTANLI_LITE_LP=ON` halves the runtime.** Dropping the propto family
entirely takes `libstanli` from 14.9 MB to 7.79 MB stripped, at the cost
of an `lp__` that sits a per-model constant above CmdStan's. Every
gradient stays bit-identical; a pinned seed draws a different but equally
valid chain, because the sampler adds `lp` to the kinetic energy and a
shifted `lp` rounds differently there. On by default for the browser
build, off for the wheel, which keeps the exact `lp__` the differential
oracle compares against. See [docs/lite-lp.md](docs/lite-lp.md).

The interpreter and NUTS together are about 410 KB. Nearly all of the
rest is the Stan compiler and the math library, which is the trade the
design makes: ship every kernel and the compiler once so that nothing is
built on the user's machine.

Shrinking it further has been measured and declined: dead-code stripping
cannot reach inside the OCaml object (96.4% of it is reachable from its
entry points anyway), and compiling stanc3 to bytecode saves 3.7 MB at
the cost of ~8x slower model compilation.

### The browser build

A different binary with different economics: no embedded stanc3, because
the compiler ships separately as JavaScript, and `STANLI_LITE_LP` on by
default. `stanli.wasm` is 3.47 MB raw and 1.01 MB gzipped, of which
3.33 MB is code, attributed the same way:

| | | |
| --- | ---: | ---: |
| stanli itself | 1.35 MB | 40.5% |
| densities and distribution functions | 0.95 MB | 28.4% |
| stan-math, everything else | 0.42 MB | 12.5% |
| Eigen (out-of-line) | 0.26 MB | 7.7% |
| libc++, NUTS, runtime, unattributed | 0.21 MB | 6.4% |
| SUNDIALS | 0.06 MB | 1.7% |
| nlohmann/json | 0.05 MB | 1.5% |
| Boost | 0.05 MB | 1.4% |

The densities are 28% here against 54% in the wheel, and stanli's own
code leads instead. Two reasons: the propto instantiations are gone, and
`densities.cpp` is the one file the browser build compiles at `-Oz`
rather than `-O3`, which was worth 8.5 MB of wasm and costs about 10% on
a mixture model. What is left of stanli grows as a share because the
interpreter and the register machine are templates that survive both.

stanc3 as JavaScript is a separate 2.84 MB, 0.40 MB gzipped, and a page
that ships precompiled MIR never fetches it. So the floor for sampling a
known model in a browser is the 1.01 MB runtime alone.

**The browser `lp__` is not CmdStan's.** `STANLI_LITE_LP` is what makes
the numbers above, and it drops the propto instantiations, so `~`
evaluates the full density and `lp__` lands a per-model constant above
CmdStan's. Gradients and every `write_array` value stay bitwise
identical, and the posterior is the same posterior, but do not compare a
browser `lp__` against a CmdStan run or feed it to anything reading log
densities as absolute numbers. The wheels ship the exact build.
`stanli_exact_lp()` reports which one is loaded. Details in
[docs/lite-lp.md](docs/lite-lp.md).

## Python

A ctypes wrapper over the same shared library, published to PyPI as one
platform wheel per platform.

```
pip install stanli                 # or: ./tools/build_wheel.sh
```

```python
import stanli
m = stanli.Model(stan_file="model.stan", data={"J": 8, "y": y, "sigma": s})
draws = m.sample(seed=1, warmup=1000, samples=1000)
draws["mu"].mean()
```

Builds without the embedded stanc3 object fall back to running a bundled
stanc binary as a subprocess.

## Browser (WASM)

The same runtime compiles to WebAssembly and runs full Stan in a browser
tab with no server: **[seantalts.github.io/stanli](https://seantalts.github.io/stanli/)**.
stanc3's js_of_ocaml build compiles the model to MIR in JS, and
stanli.wasm lowers it and samples. Eight schools goes from
Stan source to 1,000 posterior draws in about 120 ms in-tab. 118 of the
119 compiling corpus models replay against the CmdStan references from
inside WASM (`tools/wasm_check.sh` adapts `verify_refs.py`; the one
exception is `nn_rbm1bJ100`, whose compile does not fit in wasm32's 4 GB).

```
./tools/build_web.sh              # emsdk + opam builds, assembled in web/
python3 -m http.server -d web     # then open http://localhost:8000
```

Needs `deps/emsdk` (see tools/build_web.sh) and, for the stanc side, the
same opam switch that builds the embedded compiler.

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
four run on every push and pull request, so the release path is the path
that is already exercised continuously; Windows runs after the merge and
nightly, because mingw compiles the density kernels slowly enough to set
the pace of every merge on its own. A release tag runs all five, and the
publish job waits for them.
Each build links the embedded stanc3 object (cached, since it takes half
an hour to produce), runs the test suite, checks that the platform tag
matches what the library actually requires, and installs the wheel into a
clean venv to sample eight schools.

To cut a release: bump `__version__` in `python/stanli/__init__.py` (the
one place it lives; `setup.py` and the workflow both read it from there),
add the entry to `CHANGELOG.md`, then tag. The publish job fires only on
`refs/tags/v*`, asserts the tag agrees with the packaged version, and
uploads through PyPI trusted publishing, so no API token exists anywhere
in the repo or its secrets. The `pypi` deployment environment is
restricted to `v*` tags as a second lock on that.

```
git tag -a v0.1.0 -m "stanli 0.1.0" && git push origin v0.1.0
```

No sdist is published. Building from source needs a 30-minute OCaml
toolchain step, so an sdist would only turn "no wheel for your platform"
into a confusing build failure.

## Verification policy

Nothing ships on "looks close". Kernel gradients are bitwise-tested
against stan-math's var path at fixed points; whole models are
differentially verified against CmdStan (same generated model, same
deterministic evaluation point, `tools/verify_sample.py`). The corpus
scoreboard (`tools/corpus.py`) tracks which posteriordb models compile,
evaluate, and verify. Details in
[docs/corpus-status.md](docs/corpus-status.md).

## Status

26/26 tests green, built and tested in CI on macOS (arm64, x86_64) and
manylinux (x86_64, aarch64). 119/120 posteriordb models compile and
evaluate, <!--gen:corpus_verified_n-->118<!--/gen--> of them
CmdStan-verified. Of the two that are not: `sir`'s
ODE solution dips ~1e-9 below a declared lower bound at every shared
evaluation point and CmdStan rejects it there too, and `kronecker_gp`
matches on lp and 436 of 438 gradients but differs on the two that flow
through eigenvectors of a nearly degenerate covariance (see the note in
the corpus status).

## Roadmap

1. Fusing adjacent elementwise chains into one pass over the arena --
   the follow-on now that the re-roll pass covers the mixture shape
   (density lanes feeding `log_mix`/`log_sum_exp` instead of the target
   fuse into an elementwise-lp density variant plus batched mixture
   kernels) and tape islands compile the leftover scalar residue into
   single ops where that is measurably cheaper than the ops.
2. A CRAN shim package. All five wheels (macOS arm64/x86_64, Linux
   x86_64/aarch64, Windows x86_64) are built and published by
   `.github/workflows/wheels.yml` on a version tag. The Windows wheel is
   built under mingw-w64 (stan-math does not build under MSVC) and
   bundles the release `stanc.exe` as a subprocess instead of the
   embedded compiler, which waits on opam's native Windows support.
3. Vectorized kernels via stan-math's varmat (SoA) overloads. Today the
   kernels mirror CmdStan's default AoS arithmetic, which is scalar for
   transcendentals and reductions (strided var access defeats Eigen's
   packet math). stan-math's `var_value<Matrix>` overloads compute over
   contiguous doubles and vectorize, and `stanc --O1` already emits them
   variable-by-variable where every use is varmat-compatible. The plan
   follows the same shape: switch kernels to mirror the varmat
   expressions function-by-function where the overload exists
   (constrains, elementwise, matvec, the common densities), verify
   differentially against `stanc --O1` CmdStan builds, and keep AoS
   parity for the rest. Profile a large-N model first to size the win;
   graph-level fusion of elementwise chains is the follow-on.
