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
[seantalts.github.io/stanli](https://seantalts.github.io/stanli/). Stan
source to posterior draws in a few hundred milliseconds, entirely
client side.

One runtime, three packages. Each is the same shared library behind a
different binding, so a model samples to the same draws from any of them.

| | | |
| --- | --- | --- |
| **Python** | `pip install stanli` | [python/README.md](python/README.md), [PyPI](https://pypi.org/project/stanli/) |
| **R** | `install.packages("stanli", repos = "https://seantalts.r-universe.dev")` then `stanli_install()` | [r/README.md](r/README.md) |
| **Browser / Node** | `npm install @seantalts/stanli` | [js/README.md](js/README.md), [npm](https://www.npmjs.com/package/@seantalts/stanli) |

- Performance vs CmdStan: [docs/benchmarks.md](docs/benchmarks.md).
  In the <!--gen:benchmark_date-->2026-09-21<!--/gen--> native run,
  <!--gen:corpus_n_grad-->315<!--/gen--> of
  <!--gen:benchmark_models-->319<!--/gen--> models produced paired gradient
  measurements: median CmdStan/Stanli ratio
  <!--gen:corpus_median-->1.72x<!--/gen-->, with
  <!--gen:corpus_at_par-->302<!--/gen--> at or above parity.
  Failed or capped runs remain in the full table.
- Why you can trust this: [TESTING.md](TESTING.md). Every oracle, what it
  gates, what it cannot see, and the known limits.
  <!--gen:corpus_reference_models-->329<!--/gen--> models share one
  three-point CmdStan reference replay; within the posteriordb subset,
  <!--gen:corpus_verified-->118/120<!--/gen--> models are verified against
  CmdStan's log density and full gradient,
  <!--gen:corpus_bitwise-->55<!--/gen--> of them bitwise identical, worst
  relative deviation <!--gen:corpus_worst-->7.1e-13<!--/gen-->.
- How this is possible, for statisticians:
  [docs/how-it-works.md](docs/how-it-works.md)
- Tutorial, three small models traced through every layer:
  [docs/lowering-walkthrough.md](docs/lowering-walkthrough.md)
- R workflows: [classroom setup](docs/teaching.md) and
  [coming from cmdstanr](docs/from-cmdstanr.md)
- Contributor map: [docs/hacking.md](docs/hacking.md). Release process:
  [RELEASING.md](RELEASING.md).

## Architecture

A Stan model does not need machine code generated for it. Every model
is a composition of a fixed vocabulary of operations: densities,
constraint transforms, linear algebra, elementwise math. stanli ships
those operations precompiled and turns each model into data, a static
graph of ops over flat buffers, built at load time and run by a small
interpreter. Compiling a model takes milliseconds.

```
model.stan + data.json
  |  stanc3 + the stanli pipeline (embedded, or an executable on Windows)
  v
optimized typed MIR (--O1)
  |  stanli OCaml encoder
  v
compact portable MIR v2 (legacy s-expressions are also accepted)
  |  decoder + lowering: runtime/src/lower.cpp
  v
op graph + preallocated value/adjoint arenas
  |  executor: forward = log density, reverse = gradient
  v
NUTS (stan::mcmc::adapt_diag_e_nuts) -> draws
```

1. **Compiler.** stanc3 parses, typechecks, and optimizes the model. The
   shared [`compiler/ocaml/`](compiler/ocaml/) pipeline selects O1 and
   encodes the typed MIR into a versioned portable format.
   [`compiler/native/`](compiler/native/) embeds that pipeline in the shared
   library; [`compiler/js/`](compiler/js/) builds the browser compiler and
   the Windows executable.

2. **Lowering** (`runtime/src/lower.cpp`). Transformed data is evaluated
   eagerly, loops with data-known bounds are unrolled, and the model block
   becomes a linear op sequence over preallocated arenas. `~` statements
   lower to the same propto and activity instantiations CmdStan's generated
   C++ uses, so dropped constants match. An ODE right-hand side, which the
   integrator calls at times of its choosing, compiles to a flat register
   machine instead (`runtime/src/ode_prog.cpp`).

3. **Graph passes** (`runtime/src/reroll.cpp` and friends). A
   per-observation loop arrives as N copies of one small op template. The
   passes rewrite those regions into the vectorized ops the kernels already
   support: constants become vectors, invariant ops hoist, indexed reads
   become gathers, and N scalar density terms fuse into one summed vector
   density. Anything a pass cannot prove safe it leaves alone. Each pass,
   its measurements, and its switch are in
   [runtime/src/OPTIMIZATIONS.md](runtime/src/OPTIMIZATIONS.md).

4. **Execution** (`runtime/src/executor.cpp`). The op graph is the AD
   tape. The forward sweep computes the log density and stashes each op's
   partials; the reverse sweep runs the ops backward, contracting adjoints.
   Steady-state gradient evaluation allocates no memory.

5. **Kernels** (`runtime/kernels/`). Native kernels mirror the Eigen
   expressions of stan-math's rev overloads, so gradients match CmdStan
   bitwise. Everything else runs as a legacy op: a recorder scalar or a
   nested var tape drives unmodified stan-math templates. Both compile
   once, when stanli is built.

6. **Sampling** (`runtime/src/nuts.cpp`). Stan's own NUTS with
   diagonal-metric adaptation, driven through a thin model adapter.

7. **Writing draws.** A second, forward-only graph lowers generated
   quantities and produces every CSV column CmdStan would write, in
   CmdStan's order and under CmdStan's naming. A section the graph cannot
   lower runs through the per-draw interpreter (`runtime/src/wa_interp.cpp`)
   with a warning naming the reason.

8. **Distribution.** Everything sits behind a C ABI
   (`runtime/include/stanli/capi.h`) in one shared library, and each binding
   is a thin wrapper over it. The macOS and Linux wheels embed the compiler
   in that library. The Windows wheel carries `stanli.dll`,
   `stanli-compile.exe`, and stock `stanc.exe` as a fallback.

## Binary size

One self-contained shared library, about 30 MB installed and 11 MB
compressed in the wheel. The densities dominate: each distribution is
instantiated once per activity mask, twice for propto, and again for the
elementwise form, which is the standing cost of shipping precompiled
math. `-DSTANLI_LITE_LP=ON` drops the propto family for a library 48%
smaller with bitwise gradients and an `lp__` that differs from CmdStan's
by a per-model constant; it is off in every shipped build, browser
included. See [docs/lp-constant.md](docs/lp-constant.md). The browser
build ships the compiler separately as JavaScript, about 3 MB raw and
425 KB gzipped, beside a 1.5 MB gzipped `stanli.wasm`. The breakdown and
how to measure it are in [docs/hacking.md](docs/hacking.md#binary-size).

## Python

```python
import stanli
m = stanli.Model(stan_file="model.stan", data={"J": 8, "y": y, "sigma": s})
fit = m.sample(seed=1, chains=4, warmup=1000, samples=1000)
fit["mu"].mean()          # every draw, chains concatenated
print(fit.summary())      # stansummary's table
print(fit.diagnose())     # the convergence checks, in words
```

Four chains by default, run in parallel. Each chain owns its executor
and its RNG stream, so the draws are byte-identical to a sequential run.
Pure Stan functions are callable directly through `stanli.Function`.
Full documentation in [python/README.md](python/README.md).

## R

```r
library(stanli)
stanli_install()   # one time: fetches the runtime for this platform

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)
summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
```

The package builds a 40 KB C bridge and downloads the prebuilt runtime
pinned to its release, so installing it never rebuilds stan-math. Draws
are `posterior`-shaped, and `bayesplot`, `loo`, and `tidybayes` work
directly. Full documentation in [r/README.md](r/README.md); classroom
setup in [docs/teaching.md](docs/teaching.md).

## Browser (WASM)

The same runtime compiles to WebAssembly and runs full Stan in a browser
tab with no server:
**[seantalts.github.io/stanli](https://seantalts.github.io/stanli/)**.
Eight schools goes from source to 1,000 draws in about 120 ms in-tab.
118 of the 119 compiling corpus models replay against the CmdStan
references from inside WASM (`tools/wasm_check.sh`; the exception is
`nn_rbm1bJ100`, whose compile does not fit in wasm32's 4 GB).

```
./tools/build_web.sh              # emsdk + opam builds, assembled in web/
python3 -m http.server -d web     # then open http://localhost:8000
```

Full documentation in [js/README.md](js/README.md).

Native, Python, R, and `stanli_run` can opt into within-chain
`reduce_sum` parallelism: [docs/native-reduce-sum.md](docs/native-reduce-sum.md).

## Building

```
./tools/dev_setup.sh               # pinned deps, build, tests
./tools/dev_setup.sh --corpus      # + posteriordb and CmdStan
```

Build recipes, platform notes, and the C++ entry points are in
[docs/hacking.md](docs/hacking.md#building).
