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

Stan `#include` directives search the directory containing `stan_file`. Add
shared directories with `include_paths`; they are searched in order, then the
file's directory (the current directory for `stan_code`):

```python
model = stanli.Model(stan_file="models/model.stan", data="data.json",
                     include_paths=["shared", "vendor/stan"])
```

A constructed model keeps its compiled includes, so later sampling does not
reread the files.

`Model(..., fast_math=True)` opts in to
[fast mode](https://github.com/seantalts/stanli/blob/main/docs/fast-mode.md):
faster gradients, with results that can differ from the default in the last
bits.

## Sampling and diagnostics

Four chains run in parallel by default. R-hat needs more than one chain. Each
chain owns its executor and RNG stream, so the draws are byte-identical to a
sequential run. Eight schools runs all four chains in about 70 ms.

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

`refresh=0` silences it; another positive integer changes the interval.
Output goes through `sys.stdout`, so notebooks and
`contextlib.redirect_stdout()` work. Progress never changes draws or RNG
streams. If any post-warmup transition diverges or hits the maximum treedepth,
the final output reports the count. Ctrl-C stops every chain after its current
transition and raises `KeyboardInterrupt`.

```python
print(fit.summary())
```

```
name                Mean       MCSE     StdDev         5%        50%        95%   ESS_bulk   ESS_tail      R_hat
mu                4.4600     0.0532     3.1705    -0.7414     4.5519     9.5384       3586       2847      1.000
tau               3.4752     0.0635     3.1612     0.2192     2.6680     9.6313       2160       1874      1.001
```

R-hat is rank-normalized split R-hat and ESS is the bulk/tail pair
(Vehtari et al. 2021), computed by Stan's own estimators, so the numbers match
`stansummary`.

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

E-BFMI catches a badly explored heavy tail that R-hat and ESS both miss.
`fit.divergences`, `fit.max_treedepth_hits`, `fit.stepsize` and `fit.ebfmi()`
are per-chain arrays, and `fit.to_arviz()` returns an ArviZ InferenceData with
the sampler statistics attached.

### Metric and step size

`metric` is `"diag_e"` (the default), `"dense_e"` or `"unit_e"`, as in CmdStan.
`inv_metric` sets the starting inverse metric: shape `(n,)` for `"diag_e"` or
`(n, n)` for `"dense_e"`, shared by every chain, or one per chain. `step_size`
sets the initial step size (default 1). `adapt_engaged=False` turns adaptation
off, so the metric and step size stay as given.

After sampling, `fit.metric`, `fit.step_size` and `fit.inv_metric` hold what
each chain adapted. A pilot fit can start a later one:

```python
pilot = model.sample(seed=1, metric="dense_e")
fit = model.sample(seed=2, metric="dense_e", warmup=0, adapt_engaged=False,
                   inv_metric=pilot.inv_metric,
                   step_size=float(pilot.step_size.mean()))
```

With fewer than 150 warmup iterations Stan does not adapt the metric, and a
supplied one is kept.

### Within-chain parallelism

For models using `reduce_sum` or `reduce_sum_static`,
`model.sample(threads_per_chain=4, parallel_chains=2)` runs eligible
reductions on up to eight threads. Small or unsupported reductions stay
serial; `model.reduce_sum_count` and `model.reduce_sum_fallbacks` report what
was kept. Changing the thread count rebuilds the model and can change rounding
and therefore the draws. See
[native reductions](https://github.com/seantalts/stanli/blob/main/docs/native-reduce-sum.md).

## Other algorithms

`optimize()` runs Stan's L-BFGS and returns the posterior mode:

```python
r = model.optimize(seed=1)
r["mu"], r.lp          # every CSV column at the mode, and the lp there
r.unconstrained        # the point on the sampler's scale

fit = model.sample(inits=r.unconstrained)   # start the chains there
```

`pathfinder_init` fits single-path Pathfinder and draws one NUTS start per
chain from it. The sampling seed controls both stages, and `{}` uses CmdStan's
defaults:

```python
fit = model.sample(chains=4, seed=303,
                   pathfinder_init={"num_iterations": 500, "num_elbo_draws": 25})
```

The other options are `history_size` and Pathfinder's own `init_radius`. It
cannot be combined with `inits`, and a single path does no PSIS resampling.

Standalone Pathfinder, ADVI and Laplace draws are available from the
[R package](https://github.com/seantalts/stanli/blob/main/r/README.md#other-algorithms)
only.

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
empty integer argument. Complex, void, RNG and `_lp` functions are not
callable this way. Reuse a `Function` handle across calls; its lookup tables
are built once.

## API reference

```python
import stanli

model = stanli.Model(stan_file="model.stan", data="data.json")
model = stanli.Model(stan_code=src, data={"J": 8, "y": y, "sigma": sigma},
                     include_paths=None, fast_math=False)

model.n_unconstrained               # length of the unconstrained vector
model.constrained_names             # ['mu', 'tau', 'theta[1]', ...]
model.unconstrain({"mu": 0.5})      # constrained values -> unconstrained vector
lp, grad = model.log_prob_grad(q)   # log density and gradient at q

fit = model.sample(chains=4, seed=1, warmup=1000, samples=1000, delta=0.8,
                   max_depth=10, thin=1, save_warmup=False, inits=None,
                   init_radius=2.0, pathfinder_init=None,
                   parallel_chains=None, refresh=100, threads_per_chain=1,
                   metric="diag_e", inv_metric=None, step_size=None,
                   adapt_engaged=True)
fit["theta"]                        # (chains*draws, 8), declared shape
fit["theta[1]"]                     # one column, chains concatenated
fit["lp__"]                         # sampler columns by name too
fit.summary(); fit.diagnose(); fit.to_arviz()

r = model.optimize(seed=1, iter=2000, init=None, init_radius=2.0)

f = stanli.Function("name", stan_file="functions.stan")
mir = stanli.stan_to_mir(src, include_paths=None, fast_math=False)
bs = stanli.bridgestan_model(stan_file="model.stan", data="data.json")
```

`sample` returns every column CmdStan's CSV would carry: parameters,
transformed parameters and generated quantities, named the way CmdStanPy
names them (`theta[1]` through `theta[8]` for a `vector[8] theta`). Chain `c`
uses CmdStan's RNG stream for `(seed, c + 1)`.

`inits` are on the unconstrained scale: one vector for every chain, or one row
per chain. Pass constrained values through `model.unconstrain()` first.
`init_radius=0` starts every chain at the origin, CmdStan's `init=0`.

## Platforms

Wheels for macOS (arm64, x86_64), Linux (x86_64, aarch64, manylinux_2_28) and
Windows (x86_64). The Windows wheel is built with mingw-w64, because stan-math
does not build under MSVC (RStan ships through RTools for the same reason).
On Windows the compiler runs as a short-lived `stanli-compile.exe` subprocess.

## Limits

- `optimize(jacobian=False)`, CmdStan's default penalized maximum likelihood,
  raises. The change-of-variables Jacobian is part of the compiled graph, so
  `optimize()` always returns the posterior mode.
- Standalone Pathfinder, ADVI, Laplace and fixed-parameter sampling are in the
  R package only.
- Standalone generated quantities over an earlier fit's draws are not
  available.

## More

- How it works: [docs/how-it-works.md](https://github.com/seantalts/stanli/blob/main/docs/how-it-works.md).
- Correctness: <!--gen:corpus_verified_of-->118 of 120<!--/gen--> posteriordb
  models match CmdStan's log density and every gradient component,
  <!--gen:corpus_bitwise-->55<!--/gen--> of them bitwise, with a worst
  relative error of <!--gen:corpus_worst-->7.1e-13<!--/gen-->. See
  [TESTING.md](https://github.com/seantalts/stanli/blob/main/TESTING.md).
- Speed: in the <!--gen:benchmark_date-->2026-10-20<!--/gen--> run,
  <!--gen:corpus_n_grad-->336<!--/gen--> of
  <!--gen:benchmark_models-->343<!--/gen--> models produced paired gradient
  timings. The median gradient is <!--gen:corpus_median-->2.20x<!--/gen-->
  faster than CmdStan's, and <!--gen:corpus_at_par-->324<!--/gen--> are at
  least as fast. See
  [docs/benchmarks.md](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md).
- Source and issues: [github.com/seantalts/stanli](https://github.com/seantalts/stanli).
  License: BSD-3-Clause, matching Stan's.
