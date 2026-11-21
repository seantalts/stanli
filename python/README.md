# stanli

**The Stan Language Interpreter.** Compile and sample Stan models with no
C++ toolchain on the machine.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![Python](https://img.shields.io/pypi/pyversions/stanli.svg)](https://pypi.org/project/stanli/)
[![License](https://img.shields.io/pypi/l/stanli.svg)](https://github.com/seantalts/stanli/blob/main/LICENSE)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)

## Install

```console
pip install stanli
```

That is the whole install. There is no compiler, no `make`, no CmdStan
checkout, and no first-run build. The wheel is about 11 MB and holds one
shared library with the Stan compiler, stan-math and the sampler.

## Quick start

```python
import stanli

model = stanli.Model(stan_file="eight_schools.stan", data="data.json")
fit = model.sample(seed=1, chains=4, warmup=1000, samples=1000)

fit["mu"].mean()        # every draw of a column, chains concatenated
fit.draws("mu")         # (chains, draws), for a trace plot
```

`stan_code=` takes the model source as a string. `data` is a path to a JSON
file or a dict of Python scalars, lists and NumPy arrays.

Stan `#include` directives search the directory that contains `stan_file`.
To add shared directories, pass `include_paths`. stanli searches them in
order, then the file's directory (the current directory for `stan_code`):

```python
model = stanli.Model(stan_file="models/model.stan", data="data.json",
                     include_paths=["shared", "vendor/stan"])
```

A model keeps its compiled includes, so stanli does not read the included
files again when you sample.

`Model(..., fast_math=True)` turns on
[fast mode](https://github.com/seantalts/stanli/blob/main/docs/fast-mode.md).
Gradients are faster, and results can differ from the default in the last
bits.

## Sampling and diagnostics

Four chains run in parallel by default. R-hat needs more than one chain. Each
chain has its own executor and RNG stream, so the draws are byte-identical to
a sequential run. Sampling four chains of Eight Schools takes about 70 ms.

Sampling prints CmdStan-style progress every 100 transitions, then per-chain
times:

```text
Chain [1] Iteration:    1 / 2000 [  0%]  (Warmup)
Chain [1] Iteration: 1000 / 2000 [ 50%]  (Warmup)
Chain [1] Iteration: 1001 / 2000 [ 50%]  (Sampling)
Chain [1] Iteration: 2000 / 2000 [100%]  (Sampling)

Chain [1] Elapsed Time: 0.821 seconds (Warm-up)
                        0.169 seconds (Sampling)
                        0.990 seconds (Total)
```

`refresh=0` turns progress, timing and the divergence summary off, and another
positive integer changes the interval. The sampler's own notices, such as a
rejected proposal, print either way, as in CmdStan; `show_messages=False`
drops them, along with anything the model prints. Output goes through `sys.stdout`, so notebooks and
`contextlib.redirect_stdout()` work. Printing progress does not change the
draws or the RNG streams. Once a chain has post-warmup divergences or
transitions that hit the maximum treedepth, its sampling lines show the
counts so far:

```text
Chain [1] Iteration: 1100 / 2000 [ 55%]  (Sampling)  3 divergent, 12 at max treedepth (10)
```

At the end, stanli prints CmdStan's warning if any post-warmup transition
diverges or hits the maximum treedepth, or if a chain's E-BFMI is below 0.3.
If the model throws an error and the sampler rejects a proposal, stanli
reports it with the chain number, as CmdStan does. Ctrl-C stops every chain
after its current transition and raises `KeyboardInterrupt`.

```python
print(fit.summary())
```

```
name                Mean       MCSE     StdDev         5%        50%        95%   ESS_bulk   ESS_tail      R_hat
mu                4.4600     0.0532     3.1705    -0.7414     4.5519     9.5384       3586       2847      1.000
tau               3.4752     0.0635     3.1612     0.2192     2.6680     9.6313       2160       1874      1.001
```

R-hat is rank-normalized split R-hat, and ESS is the bulk and tail pair
(Vehtari et al. 2021). Stan's own estimators compute them, so the numbers
match `stansummary`.

```python
print(fit.diagnose())
```

```
No divergent transitions.
No transitions saturated the maximum treedepth of 10.
E-BFMI is above 0.3 in every chain.
R-hat is below 1.01 for every parameter (worst 1.002, theta.6).
Bulk ESS is at least 100 per chain for every parameter (worst 2160, tau).
Tail ESS is at least 100 per chain for every parameter (worst 1874, tau).
No problems detected.
```

E-BFMI can detect a heavy tail that the sampler explored poorly, which R-hat
and ESS both miss. `fit.divergences`, `fit.max_treedepth_hits`,
`fit.stepsize` and `fit.ebfmi()` are per-chain arrays, and `fit.to_arviz()`
returns an ArviZ InferenceData with the sampler statistics attached.

### Metric and step size

`metric` is `"diag_e"` (the default), `"dense_e"` or `"unit_e"`, as in CmdStan.
`inv_metric` sets the starting inverse metric: shape `(n,)` for `"diag_e"` or
`(n, n)` for `"dense_e"`, shared by every chain, or one per chain. `step_size`
sets the initial step size (default 1). `adapt_engaged=False` turns adaptation
off, so the metric and step size stay as given. `adapt_metric=False` adapts
the step size only and keeps the `inv_metric` that you supply. Use it, for
example, with a metric built from the Hessian at the mode.

After sampling, `fit.metric`, `fit.step_size` and `fit.inv_metric` hold what
each chain adapted. You can use them to start a new fit from a pilot fit:

```python
pilot = model.sample(seed=1, metric="dense_e")
fit = model.sample(seed=2, metric="dense_e", warmup=0, adapt_engaged=False,
                   inv_metric=pilot.inv_metric,
                   step_size=float(pilot.step_size.mean()))
```

With fewer than 150 warmup iterations, Stan does not adapt the metric and
keeps the one that you supply. With `warmup=0` and no `adapt_engaged` given,
sampling runs at exactly `step_size`; `adapt_engaged=True` with `warmup=0` is
an error, as in CmdStan.

### More draws from the same chains

`continue_from=fit` continues each chain from where it stopped. Each chain
keeps its step size, metric and random stream, and there is no new warmup.
The call returns a new fit with the new draws appended. The result is the
same, bit for bit, as asking for all the samples in the first call:

```python
fit = model.sample(seed=1, samples=1000)
fit = model.sample(continue_from=fit, samples=1000)   # 2000 per chain
```

Only `samples`, `refresh`, `show_messages` and `parallel_chains` can be
given with `continue_from`; everything else comes from the fit.

### Within-chain parallelism

For models that use `reduce_sum` or `reduce_sum_static`, set the thread count
when you build the model:

```python
model = stanli.Model(stan_file="model.stan", data="data.json",
                     threads_per_chain=4)
fit = model.sample(parallel_chains=2)   # eligible reductions on up to 8 threads
```

Small or unsupported reductions stay serial. `model.reduce_sum_count` and
`model.reduce_sum_fallbacks` report what was kept. To use another thread
count, build a new model. Rounding can then differ, so the draws can change.
Passing `threads_per_chain` to `sample()` is deprecated: it warns and
rebuilds the model for that count. See
[native reductions](https://github.com/seantalts/stanli/blob/main/docs/native-reduce-sum.md).

## Other algorithms

`optimize()` runs Stan's L-BFGS and returns the posterior mode:

```python
r = model.optimize(seed=1)
r["mu"], r.lp          # every CSV column at the mode, and the lp there
r.unconstrained        # the point on the sampler's scale

fit = model.sample(inits=r.unconstrained)   # start the chains there
```

`pathfinder_init` runs single-path Pathfinder and draws a NUTS starting
point for each chain from it. The sampling seed controls both stages, and
`{}` uses CmdStan's defaults:

```python
fit = model.sample(chains=4, seed=303,
                   pathfinder_init={"num_iterations": 500, "num_elbo_draws": 25})
```

The other options are `history_size` and Pathfinder's own `init_radius`. You
cannot combine `pathfinder_init` with `inits`. A single path does no PSIS
resampling.

Standalone Pathfinder, ADVI and Laplace draws are available only from the
[R package](https://github.com/seantalts/stanli/blob/main/r/README.md#other-algorithms).

## Call a Stan function from Python

`Function` calls a pure, value-returning Stan user-defined function without
building a model. The source compiles once:

```python
source = """
functions {
  vector affine(vector x, real a, real b) {
    return a * x + b;
  }
}
model {}
"""
affine = stanli.Function("affine", stan_code=source)
affine(x=[1, 2, 4], a=2.5, b=-1)  # array([1.5, 4.0, 9.0])
```

Use `stan_file=` instead of `stan_code`, or pass
`mir=stanli.stan_to_mir(source)` to reuse a compilation. Calls take keyword
arguments or one mapping. Scalars come back as `float` or `int`; vectors,
matrices and arrays come back as NumPy arrays with the declared shape.
Integers must fit Stan's 32-bit range. Select an ambiguous overload by its
resolved name, such as `f(real,vector)`, and pass an integer-dtype array for an
empty integer argument. You cannot call complex, void, RNG or `_lp`
functions this way. Reuse a `Function` handle across calls, because stanli
builds its lookup tables only once.

## API reference

```python
import stanli

model = stanli.Model(stan_file="model.stan", data="data.json")
model = stanli.Model(stan_code=src, data={"J": 8, "y": y, "sigma": sigma},
                     threads_per_chain=1, include_paths=None,
                     fast_math=False, allow_interpreter=True)

model.n_unconstrained               # length of the unconstrained vector
model.constrained_names             # ['mu', 'tau', 'theta[1]', ...]
model.unconstrain({"mu": 0.5})      # constrained values -> unconstrained vector
lp, grad = model.log_prob_grad(q)   # log density and gradient at q
lp, grad, hess = model.log_prob_hessian(q)   # and the (n, n) Hessian at q

fit = model.sample(chains=4, seed=1, warmup=1000, samples=1000, delta=0.8,
                   max_depth=10, thin=1, save_warmup=False, inits=None,
                   init_radius=2.0, pathfinder_init=None,
                   parallel_chains=None, refresh=100, show_messages=True,
                   metric="diag_e", inv_metric=None, step_size=None,
                   adapt_engaged=None, adapt_metric=True, continue_from=None)
fit["theta"]                        # (chains*draws, 8), declared shape
fit["theta[1]"]                     # one column, chains concatenated
fit["lp__"]                         # sampler columns by name too
fit.summary(); fit.diagnose(); fit.to_arviz()

r = model.optimize(seed=1, iter=2000, init=None, init_radius=2.0)

f = stanli.Function("name", stan_file="functions.stan")
mir = stanli.stan_to_mir(src, include_paths=None, fast_math=False)
bs = stanli.bridgestan_model(stan_file="model.stan", data="data.json")
lib, text = stanli.runtime_path(), stanli.bridgestan_data(stan_file="model.stan")
```

`sample` returns every column that CmdStan's CSV would contain. These are
the parameters, transformed parameters and generated quantities. Columns are
named the way CmdStanPy names them (`theta[1]` through `theta[8]` for a
`vector[8] theta`). Chain `c` uses CmdStan's RNG stream for `(seed, c + 1)`.

`inits` are on the unconstrained scale: one vector for every chain, or one row
per chain. Pass constrained values through `model.unconstrain()` first.
`init_radius=0` starts every chain at the origin, CmdStan's `init=0`.

You can run a stanli model from any BridgeStan client, including nutpie.
Pass `runtime_path()` as the model library and `bridgestan_data()` as the
data argument. `bridgestan_data()` returns JSON text that tells the runtime
which program to serve. `bridgestan_model()` does both steps for the
`bridgestan` package.

## Platforms

Wheels are available for macOS (arm64, x86_64), Linux (x86_64, aarch64,
manylinux_2_28) and Windows (x86_64). The Windows wheel is built with
mingw-w64, because stan-math does not build under MSVC (RStan ships through
RTools for the same reason).
On Windows the compiler runs as a short-lived `stanli-compile.exe` subprocess.

## Limits

- `optimize(jacobian=False)`, CmdStan's default penalized maximum likelihood,
  raises an error. The change-of-variables Jacobian is part of the compiled
  graph, so `optimize()` always returns the posterior mode.
- Standalone Pathfinder, ADVI, Laplace and fixed-parameter sampling are in the
  R package only.
- Standalone generated quantities over an earlier fit's draws are not
  available.

## More

- How it works: [docs/how-it-works.md](https://github.com/seantalts/stanli/blob/main/docs/how-it-works.md).
- Correctness: <!--gen:corpus_verified_of-->118 of 120<!--/gen--> posteriordb
  models match CmdStan's log density and every gradient component. Of
  these, <!--gen:corpus_bitwise-->55<!--/gen--> match bitwise. The worst
  relative error is <!--gen:corpus_worst-->7.1e-13<!--/gen-->. See
  [TESTING.md](https://github.com/seantalts/stanli/blob/main/TESTING.md).
- Speed: in the benchmark run of <!--gen:benchmark_date-->2026-11-19<!--/gen-->,
  we timed gradients in both stanli and CmdStan
  for <!--gen:corpus_n_grad-->362<!--/gen--> of <!--gen:benchmark_models-->363<!--/gen-->
  models. The median gradient is <!--gen:corpus_median-->2.16x<!--/gen-->
  faster than CmdStan's, and <!--gen:corpus_at_par-->346<!--/gen--> models are
  at least as fast. See
  [docs/benchmarks.md](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md).
- Source and issues: [github.com/seantalts/stanli](https://github.com/seantalts/stanli).
  License: BSD-3-Clause, matching Stan's.
