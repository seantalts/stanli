# stanli

**The Stan Language Interpreter.** Compile and sample Stan models with no
C++ toolchain on the machine.

[![PyPI](https://img.shields.io/pypi/v/stanli.svg)](https://pypi.org/project/stanli/)
[![Python](https://img.shields.io/pypi/pyversions/stanli.svg)](https://pypi.org/project/stanli/)
[![License](https://img.shields.io/pypi/l/stanli.svg)](https://github.com/seantalts/stanli/blob/main/LICENSE)
[![wheels](https://github.com/seantalts/stanli/actions/workflows/wheels.yml/badge.svg)](https://github.com/seantalts/stanli/actions/workflows/wheels.yml)

```console
pip install stanli
```

That is the whole install. No compiler, no `make`, no CmdStan checkout,
no multi-minute first-run build. One wheel, one shared library, about
11 MB compressed. Models run without a per-model C++ build; see the
[current benchmark](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md)
for measured gradients and setup-plus-20,000-gradient time estimates.

```python
import stanli

model = stanli.Model(stan_file="eight_schools.stan", data="data.json")
fit = model.sample(seed=1, chains=4, warmup=1000, samples=1000)

fit["mu"].mean()        # every draw of a column, chains concatenated
fit.draws("mu")         # (chains, draws), for a trace plot
```

Stan `#include` directives automatically search the directory containing
`stan_file`. Add shared directories with `include_paths`:

```python
model = stanli.Model(stan_file="models/model.stan", data="data.json",
                     include_paths=["shared", "vendor/stan"])
```

Explicit directories are searched in order, then the input file's directory
(the current directory for `stan_code`). Nested includes use the same search
path, and `Function`, `stan_to_mir`, and `bridgestan_model` accept
`include_paths` too. A constructed model keeps its compiled includes, so later
sampling does not reread the files.

Sampling reports CmdStan-shaped progress every 100 transitions by default,
followed by per-chain warm-up, sampling, and total times:

```text
Chain [1] Iteration:    1 / 2000 [  0%]  (Warmup)
Chain [1] Iteration: 1000 / 2000 [ 50%]  (Warmup)
Chain [1] Iteration: 1001 / 2000 [ 50%]  (Sampling)
Chain [1] Iteration: 2000 / 2000 [100%]  (Sampling)

Chain [1] Elapsed Time: 0.821 seconds (Warm-up)
                        0.169 seconds (Sampling)
                        0.990 seconds (Total)
```

Set `refresh=0` for a quiet run, or another positive integer to change the
interval. Progress goes through Python's `sys.stdout`, so notebooks and
`contextlib.redirect_stdout()` work. Reporting only observes completed
transitions and never changes draws, sampler statistics, or RNG streams. If
any post-warmup transition diverges or saturates the maximum treedepth, the
final output reports the count over all transitions, thinned ones included.

For models using `reduce_sum` or `reduce_sum_static`,
`model.sample(threads_per_chain=4, parallel_chains=2)` opts into native
within-chain parallelism, up to eight sampling threads here. The default is
one thread per chain; small or unsupported reductions stay serial, and
`model.reduce_sum_count` and `model.reduce_sum_fallbacks` report what was
retained. Changing the thread setting rebuilds the prepared model and can
change reduction rounding and NUTS draws. See the
[native reduction guide](https://github.com/seantalts/stanli/blob/main/docs/native-reduce-sum.md).

## Chains and convergence

Four chains by default, run in parallel, because R-hat needs more than
one and a single-chain run cannot be checked for convergence at all.
Eight schools does all four in about 70 ms. Scheduling chains changes nothing
about the answer: each chain owns its executor and its RNG stream, so
the draws come out byte-identical to a sequential run.

```python
print(fit.summary())
```

```
name                Mean       MCSE     StdDev         5%        50%        95%   ESS_bulk   ESS_tail      R_hat
mu                4.4600     0.0532     3.1705    -0.7414     4.5519     9.5384       3586       2847      1.000
tau               3.4752     0.0635     3.1612     0.2192     2.6680     9.6313       2160       1874      1.001
```

R-hat is rank-normalized split-R-hat and ESS is the bulk/tail pair
(Vehtari et al. 2021), computed by stan's own estimators, so the
numbers agree with `stansummary` rather than approximating it.

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

These are the checks a Bayesian workflow turns on, including E-BFMI,
which catches a badly explored heavy tail that R-hat and ESS both miss.
The pieces are reachable individually too:
`fit.divergences`, `fit.max_treedepth_hits`, `fit.stepsize` and
`fit.ebfmi()` are per-chain arrays, and `fit.to_arviz()` hands off an
InferenceData with the sampler stats attached.

## The mode, and where to start

```python
r = model.optimize(seed=1)
r["mu"], r.lp          # every CSV column at the mode, and the lp there
r.unconstrained        # the point on the sampler's scale

fit = model.sample(inits=r.unconstrained)   # start the chains there
```

L-BFGS, stan's own, the one behind CmdStan's `optimize`. It returns the
posterior **mode**. CmdStan's `optimize` defaults to `jacobian=0`, the
penalized maximum likelihood, and stanli cannot offer that: the
change-of-variables Jacobian is folded into the graph when the model is
lowered. `jacobian=False` raises rather than quietly handing back the
other quantity.

## Call a Stan function from Python

`Function` exposes a pure, value-returning Stan user-defined function without
building a model or compiling C++. Source compilation happens once:

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

Use `stan_file="functions.stan"` instead of `stan_code`, or pass
`mir=stanli.stan_to_mir(source)` to reuse a cached compilation. Calls take
keyword arguments or one mapping. Scalars return `float`/`int`; vectors,
matrices, and arrays return NumPy arrays with the declared shape, and inputs
may be lists or NumPy arrays. Integers must fit Stan's 32-bit range and
promote to real formals; select an ambiguous overload by resolved name such
as `f(real,vector)`, and pass an integer-dtype array for an empty integer
argument. This is the value-only interpreter: complex, void, RNG, and `_lp`
entry points are outside it.

Reuse a `Function` handle across calls: its native lookup tables are cached
at construction, and exact Python `float` and `int` arguments take a direct
scalar path. From a repository checkout,
`python tools/bench_python_function.py` compares an installed wheel's
steady-state calls with plain Python and NumPy.

## Correctness

Nothing here ships on "looks close".

**<!--gen:corpus_verified_of-->118 of 120<!--/gen--> posteriordb models**
are differentially verified against CmdStan: same model, same data, same
evaluation point, comparing the log density and every single gradient
component. **<!--gen:corpus_bitwise-->55<!--/gen--> agree bitwise.** The
worst deviation among these verified posteriordb models is
**<!--gen:corpus_worst-->7.1e-13<!--/gen--> relative**.

The two exceptions are documented rather than hidden. `sir`'s ODE
solution dips about 1e-9 below a declared lower bound at the shared
evaluation point, where CmdStan rejects it too; `kronecker_gp` matches
on the log density and 436 of 438 gradients, differing on the two that
flow through eigenvectors of a nearly degenerate covariance matrix.

Full per-model accuracy table:
[docs/corpus-status.md](https://github.com/seantalts/stanli/blob/main/docs/corpus-status.md)

## Performance

In the <!--gen:benchmark_date-->2026-09-21<!--/gen--> native run,
<!--gen:corpus_n_grad-->315<!--/gen--> of
<!--gen:benchmark_models-->319<!--/gen--> models produced paired gradient
measurements. The median CmdStan/Stanli ratio was
<!--gen:corpus_median-->1.72x<!--/gen-->, with
<!--gen:corpus_at_par-->302<!--/gen--> at or above parity.

Stanli avoids a per-model C++ build and can combine repeated work into fewer
runtime operations. Dense kernels and serial dependencies offer fewer such
opportunities; the model and data determine the result.

The [current full table and method](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md)
include every model, failed or capped measurement, and the explicit cost-estimate formula.
The [loop-vectorized comparison](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md#against-cmdstan-with-loop-vectorization)
compares against CmdStan with stanc3 O1 and loop vectorization enabled.

## API

The surface is small on purpose.

```python
import stanli

# A path to a .stan file, or the model source directly.
model = stanli.Model(stan_file="model.stan", data="data.json")
model = stanli.Model(stan_code=src, data={"J": 8, "y": y, "sigma": sigma})

model.n_unconstrained               # length of the unconstrained vector
model.constrained_names             # ['mu', 'tau', 'theta[1]', ...]

lp, grad = model.log_prob_grad(q)   # sampling log density and its gradient

fit = model.sample(seed=1, warmup=1000, samples=1000, delta=0.8,
                   refresh=100)
fit["theta"]                        # (chains*draws, 8) ndarray
fit["theta[1]"]                     # one column, chains concatenated
```

Ctrl-C while `sample()` runs stops every chain after its current
transition and raises `KeyboardInterrupt`.

Pathfinder can generate one initialization per chain before NUTS. The
sampling seed controls both stages; an empty options object uses CmdStan's
single-path defaults:

```python
fit = model.sample(
    chains=4,
    seed=303,
    pathfinder_init={"num_iterations": 500, "num_elbo_draws": 25},
)
```

The other supported options are `history_size` and Pathfinder's own
`init_radius`. `pathfinder_init` and explicit `inits` are mutually exclusive;
single-path Pathfinder does not perform PSIS resampling.

`data` accepts a path to a JSON file or a dict of Python scalars, lists,
and numpy arrays. `sample` returns every column CmdStan's CSV would carry,
named the way CmdStanPy names them: a `theta` declared as `vector[8]` gets
`theta[1]` through `theta[8]`, and `fit["theta"]` returns an array with the
declared shape. Sampler columns (`lp__`, `divergent__`, ...) are reachable
by name too.

## Platforms

Wheels for macOS (arm64 and x86_64), Linux (x86_64 and aarch64,
manylinux_2_28) and Windows (x86_64). The Windows wheel is built under
mingw-w64, because stan-math does not build under MSVC (the same reason
RStan ships through RTools). The Windows wheel runs `stanli-compile.exe` as
a short-lived subprocess and falls back to the stock `stanc.exe` beside it
only when the preferred executable is absent; compiler failures are reported
without retrying the other path.

The installed library is about 30 MB: over half of it is the density
kernels, about a quarter the embedded stanc3, and the interpreter and
NUTS together are about 410 KB. That is the trade this design makes:
ship the compiler and every kernel once, so nothing is ever built on
the user's machine.

## Limits

Stated plainly:

- The sampler is Stan's own NUTS with diagonal-metric adaptation, and
  `optimize()` is Stan's L-BFGS. Single-path Pathfinder is available for
  sampler initialization; a standalone Python Pathfinder result is not yet
  exposed.
- `inits` are on the unconstrained scale. `model.unconstrain({...})`
  turns constrained starting values into that vector, so unconstraining is
  a step per starting point rather than a second kind of argument.
- `optimize(jacobian=False)` (CmdStan's default penalized maximum
  likelihood) raises; see above.

What is here is verified against CmdStan model by model, and every
number on this page is reproducible from the repository.

- Source, issues, and roadmap:
  [github.com/seantalts/stanli](https://github.com/seantalts/stanli)
- License: BSD-3-Clause, matching Stan's own.
