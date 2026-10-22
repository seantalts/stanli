# stanli

Stanli is a Stan Language Interpreter. It uses upstream Stan code from Stan
Math, the Stan services and algorithms, and the Stan compiler, but it does not
require a C++ toolchain. On the median of <!--gen:benchmark_models-->343<!--/gen--> real
models, gradients are **<!--gen:corpus_median-->2.15x<!--/gen-->** faster than
CmdStan's and sampling is **<!--gen:corpus_total_median-->23x<!--/gen-->** faster
([benchmarks](docs/benchmarks.md)).

Stanli turns each model into a graph of operations over precompiled Stan Math
kernels and runs Stan's own NUTS sampler on it, with no per-model compile.
Sampling time here is setup plus a full 1,000 warmup + 1,000 draw run, so it
gains more than the gradient: CmdStan builds C++ for each model and Stanli
does not. Log densities and gradients are checked against CmdStan's on a
corpus of hundreds of models.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![npm](https://img.shields.io/npm/v/@seantalts/stanli.svg)](https://www.npmjs.com/package/@seantalts/stanli)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)
[![R](https://github.com/seantalts/stanli/actions/workflows/r.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/r.yml)
[![License](https://img.shields.io/pypi/l/stanli.svg)](LICENSE)

**Try it in your browser, no install:**
[seantalts.github.io/stanli](https://seantalts.github.io/stanli/). Eight
schools goes from Stan source to 1,000 posterior draws in about 120 ms,
entirely client side.

## Install

One runtime, three packages. Each is the same shared library behind a
different binding, so a model samples to the same draws from any of them.

| | | |
| --- | --- | --- |
| **Python** | `pip install stanli` | [python/README.md](python/README.md), [PyPI](https://pypi.org/project/stanli/) |
| **R** | `install.packages("stanli", repos = "https://seantalts.r-universe.dev")` then `stanli_install()` | [r/README.md](r/README.md) |
| **Browser / Node** | `npm install @seantalts/stanli` | [js/README.md](js/README.md), [npm](https://www.npmjs.com/package/@seantalts/stanli) |

The R package downloads its matching prebuilt runtime on first use, so
installing it never builds stan-math.

## Example

```python
import stanli
m = stanli.Model(stan_file="model.stan", data={"J": 8, "y": y, "sigma": s})
fit = m.sample(seed=1, chains=4, warmup=1000, samples=1000)
fit["mu"].mean()          # every draw, chains concatenated
print(fit.summary())      # stansummary's table
print(fit.diagnose())     # the convergence checks, in words
```

Four chains run in parallel by default. Each chain owns its executor and
its random number stream, so the draws are byte-identical to a sequential
run. The [R](r/README.md) and [JavaScript](js/README.md) guides have the
same example in their language.

## What it runs

- NUTS with diagonal, dense or unit metrics, CmdStan's adaptation, and a
  starting step size and inverse metric, in Python, R and the C API.
- Optimization in Python and R. R also runs ADVI (mean-field or
  full-rank), single-path Pathfinder, Laplace sampling and fixed-parameter
  sampling.
- Generated quantities, with every CSV column CmdStan would write.
- Pure Stan functions called directly from Python or C++.
- Opt-in [fast mode](docs/fast-mode.md) and
  [within-chain `reduce_sum`](docs/native-reduce-sum.md) threads.
- Wheels for macOS (arm64, x86_64), Linux (manylinux_2_28 x86_64 and
  aarch64) and Windows x86_64, plus WebAssembly for browsers and Node.

## What is verified, and how fast

- Speed. In the <!--gen:benchmark_date-->2026-10-21<!--/gen--> native
  run, <!--gen:corpus_n_grad-->342<!--/gen-->
  of <!--gen:benchmark_models-->343<!--/gen--> models produced paired gradient
  measurements. The median gradient
  is <!--gen:corpus_median-->2.15x<!--/gen--> faster than CmdStan's,
  and <!--gen:corpus_at_par-->327<!--/gen--> models are at least as fast.
  Failed or capped runs stay in the [full benchmark](docs/benchmarks.md).
- Model coverage. <!--gen:corpus_reference_models-->353<!--/gen--> models
  share one three-point CmdStan reference replay. Within the posteriordb
  subset, <!--gen:corpus_verified-->118/120<!--/gen--> models match
  CmdStan's log density and full gradient.
  Of those <!--gen:corpus_verified_n-->118<!--/gen-->, <!--gen:corpus_bitwise-->55<!--/gen--> are bitwise identical, and the worst
  relative deviation is <!--gen:corpus_worst-->7.1e-13<!--/gen-->. Every model
  is listed in [docs/corpus-status.md](docs/corpus-status.md).
- Language coverage. <!--gen:lang_verified-->11/11<!--/gen--> models lifted
  from stanc3's own test suite, for the type and language constructs no
  real posterior uses, are verified the same way
  ([tests/stanc3/README.md](tests/stanc3/README.md)).
- Distribution coverage. 71 of 72 densities, all 105 cdf/lcdf/lccdf
  functions, truncation, censoring, ordinal regression and the count GLMs
  are each verified against CmdStan ([docs/coverage.md](docs/coverage.md)).
- How we test. [TESTING.md](TESTING.md) lists every oracle, what it gates,
  what it cannot see, and the known limits.
- Install size. One 29.7 MB shared library in a 10.8 MB wheel
  ([docs/install-size.md](docs/install-size.md)).

## How it works

A Stan model is a composition of a fixed set of operations: densities,
constraint transforms, linear algebra and elementwise math. stanli ships
those operations precompiled. stanc3, the official Stan compiler, parses
and optimizes the model, and stanli lowers the result into a static graph
of operations over flat buffers. Graph passes rewrite per-observation
loops into vectorized operations. The graph is also the autodiff tape: a
forward sweep computes the log density and a reverse sweep computes the
gradient. Stan's NUTS sampler drives it, and a second, forward-only graph
writes every column CmdStan would write, in CmdStan's order. Loading a
model takes milliseconds. [docs/how-it-works.md](docs/how-it-works.md)
explains why this can outrun a compiled model, and
[docs/hacking.md](docs/hacking.md) maps the code.

## Documentation

The full index is [docs/README.md](docs/README.md).

### Use

- [Python](python/README.md), [R](r/README.md) and
  [JavaScript](js/README.md) package guides
- [Coming from cmdstanr](docs/from-cmdstanr.md)
- [Teaching with stanli](docs/teaching.md), including brms and
  Rethinking
- [Fast mode](docs/fast-mode.md)
- [Within-chain `reduce_sum` parallelism](docs/native-reduce-sum.md)

### Trust

- [TESTING.md](TESTING.md), every oracle and the known limits
- [Corpus status](docs/corpus-status.md) and
  [distribution coverage](docs/coverage.md)
- [Benchmarks and how we measure](docs/benchmarks.md)
- [The lp constant](docs/lp-constant.md), when `lp__` differs from
  CmdStan's

### Understand

- [How it works](docs/how-it-works.md), for statisticians
- [Lowering walkthrough](docs/lowering-walkthrough.md), three small models
  traced through every layer
- [Graph optimizations](runtime/src/OPTIMIZATIONS.md)
- [Install size](docs/install-size.md)

### Contribute

- [Hacking on stanli](docs/hacking.md), covering building, layout, the C++ API, and
  verifying a change
- [Releasing](RELEASING.md)
