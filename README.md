# stanli

Stanli is a Stan Language Interpreter. It uses upstream Stan code from Stan
Math, the Stan services and algorithms, and the Stan compiler, but it does not
require a C++ toolchain. We benchmarked it on <!--gen:benchmark_models-->363<!--/gen-->
real models. On the median model, gradients are **<!--gen:corpus_median-->2.16x<!--/gen-->**
faster than CmdStan's and sampling is **<!--gen:corpus_total_median-->23x<!--/gen-->**
faster ([benchmarks](docs/benchmarks.md)).

stanli turns each model into a graph of operations over precompiled Stan Math
kernels. Stan's own NUTS sampler then runs on that graph. No model is compiled
to C++. The sampling time includes setup and a full run of 1,000 warmup and
1,000 sampling iterations. Sampling gains more than the gradient does, because
CmdStan compiles C++ for each model and stanli does not. We check log
densities and gradients against CmdStan's on hundreds of models.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![npm](https://img.shields.io/npm/v/@seantalts/stanli.svg)](https://www.npmjs.com/package/@seantalts/stanli)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)
[![R](https://github.com/seantalts/stanli/actions/workflows/r.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/r.yml)
[![License](https://img.shields.io/pypi/l/stanli.svg)](LICENSE)

**Try it in your browser, no install:**
[seantalts.github.io/stanli](https://seantalts.github.io/stanli/). The demo
page compiles the Eight Schools model and draws 1,000 posterior samples in
about 120 ms. Everything runs in your browser.

## Install

There are three packages. Each one wraps the same shared library, so a
model gives the same draws from any of them.

| | | |
| --- | --- | --- |
| **Python** | `pip install stanli` | [python/README.md](python/README.md), [PyPI](https://pypi.org/project/stanli/) |
| **R** | `install.packages("stanli", repos = "https://seantalts.r-universe.dev")` then `stanli_install()` | [r/README.md](r/README.md) |
| **Browser / Node** | `npm install @seantalts/stanli` | [js/README.md](js/README.md), [npm](https://www.npmjs.com/package/@seantalts/stanli) |

On first use, the R package downloads a prebuilt runtime that matches its
version. You never build stan-math.

## Example

```python
import stanli
m = stanli.Model(stan_file="model.stan", data={"J": 8, "y": y, "sigma": s})
fit = m.sample(seed=1, chains=4, warmup=1000, samples=1000)
fit["mu"].mean()          # every draw, chains concatenated
print(fit.summary())      # stansummary's table
print(fit.diagnose())     # the convergence checks, in words
```

Four chains run in parallel by default. Each chain has its own executor and
random number stream, so the draws are byte-identical to a sequential run.
The [R](r/README.md) and [JavaScript](js/README.md) guides show the same
example in their language.

## What it runs

- NUTS with CmdStan's adaptation. In Python, R and the C API you can choose
  a diagonal, dense or unit metric and set the starting step size and
  inverse metric.
- Optimization in Python and R. R also runs ADVI (mean-field or
  full-rank), single-path Pathfinder, Laplace sampling and fixed-parameter
  sampling.
- Generated quantities, with every CSV column CmdStan would write.
- Calls to pure Stan functions directly from Python or C++.
- Optional [fast mode](docs/fast-mode.md) and
  [within-chain `reduce_sum`](docs/native-reduce-sum.md) threads.
- Wheels for macOS (arm64, x86_64), Linux (manylinux_2_28 x86_64 and
  aarch64) and Windows x86_64, plus WebAssembly for browsers and Node.

## What is verified, and how fast

- Speed. In the native benchmark run
  of <!--gen:benchmark_date-->2026-11-19<!--/gen-->, we timed gradients in
  both stanli and CmdStan for <!--gen:corpus_n_grad-->362<!--/gen-->
  of <!--gen:benchmark_models-->363<!--/gen--> models. The median gradient
  is <!--gen:corpus_median-->2.16x<!--/gen--> faster than CmdStan's,
  and <!--gen:corpus_at_par-->346<!--/gen--> models are at least as fast.
  The [full benchmark](docs/benchmarks.md) also lists the runs that failed or
  were capped.
- Model coverage. We compare <!--gen:corpus_reference_models-->398<!--/gen-->
  models with CmdStan's recorded answers at three points each. In the
  posteriordb subset, <!--gen:corpus_verified-->118/120<!--/gen--> models
  match CmdStan's log density and full gradient.
  Of those <!--gen:corpus_verified_n-->118<!--/gen-->,
  the results for <!--gen:corpus_bitwise-->55<!--/gen--> are bitwise
  identical. The worst relative deviation
  is <!--gen:corpus_worst-->7.1e-13<!--/gen-->.
  [docs/corpus-status.md](docs/corpus-status.md) lists every model.
- Language coverage. stanc3's own test suite covers type and language
  features that no real posterior uses. We compare every model in that suite
  with recorded CmdStan answers
  ([tests/stanc3_good/README.md](tests/stanc3_good/README.md)). We also
  verify the programs that once failed to lower, in the same way as the
  corpus. Of these, <!--gen:lang_verified-->13/13<!--/gen--> pass
  ([tests/regressions/README.md](tests/regressions/README.md)).
- Distribution coverage. We verify each of these against CmdStan: 71 of 72
  densities, all 105 cdf/lcdf/lccdf functions, truncation, censoring,
  ordinal regression and the count GLMs ([docs/coverage.md](docs/coverage.md)).
- How we test. [TESTING.md](TESTING.md) lists each reference that we compare
  against, what it checks, what it misses, and the known limits.
- Install size. The wheel is 10.8 MB and holds one 29.7 MB shared library
  ([docs/install-size.md](docs/install-size.md)).

## How it works

A Stan model is built from a fixed set of operations, such as densities,
constraint transforms, linear algebra and elementwise math. stanli ships
those operations precompiled.

stanc3, the official Stan compiler, parses and optimizes the model. stanli
then converts the result into a static graph of operations over flat
buffers. Passes over the graph rewrite per-observation loops into vectorized
operations. stanli also uses the graph for automatic differentiation. A
forward pass computes the log density and a reverse pass computes the
gradient. Stan's NUTS sampler calls this graph. A second graph, which runs
forward only, writes every column that CmdStan would write, in CmdStan's
order. Loading a model takes milliseconds.

[docs/how-it-works.md](docs/how-it-works.md) explains why this can be faster
than a compiled model, and [docs/hacking.md](docs/hacking.md) describes the
layout of the code.

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
- [lp__ and dropped constants](docs/lp-constant.md), which terms `~`
  leaves out of `lp__`

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
