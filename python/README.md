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
no multi-minute first-run build. One wheel, one shared library, under
eight megabytes. In the current Eight Schools benchmark, a first complete
1,000-warmup, 1,000-draw run takes 0.03 s in stanli versus 3.4 s to build and
run the model with CmdStan - roughly 100x faster from source to CSV.

```python
import stanli

model = stanli.Model(stan_file="eight_schools.stan", data="data.json")
fit = model.sample(seed=1, chains=4, warmup=1000, samples=1000)

fit["mu"].mean()        # every draw of a column, chains concatenated
fit.draws("mu")         # (chains, draws), for a trace plot
```

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

Set `refresh=0` for a completely quiet run, or another positive integer to
change the update interval. Progress is written through Python's `sys.stdout`,
so notebook output and `contextlib.redirect_stdout()` work normally. Reporting
only observes completed transitions: changing `refresh` does not change draws,
sampler statistics, generated-quantity RNG streams, or reproducibility. If any
post-warmup transition diverges or saturates the maximum treedepth, the final
output also reports the aggregate count. Those counts cover all transitions,
including transitions omitted by thinning.

## Chains and convergence

Four chains by default, run in parallel, because R-hat needs more than
one and a single-chain run cannot be checked for convergence at all.
Eight schools does all four in about 70 ms. Threading changes nothing
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

Those are the checks a Bayesian workflow actually turns on, including
E-BFMI, the one that catches a badly explored heavy tail, which R-hat
and ESS are both blind to. The pieces are reachable individually too:
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
`mir=stanli.stan_to_mir(source)` to reuse cached compilation. Calls take
keyword arguments or one mapping, such as `affine({"x": x, "a": 2.5, "b": -1})`.
Scalars return Python `float`/`int`; vectors, matrices, and arrays return owned
NumPy arrays with the same logical shape. Inputs can be rectangular lists or
NumPy arrays, including strided views. The adapter uses typed numeric buffers,
not JSON.

Integer inputs must fit Stan's 32-bit integers and promote to real formals.
Overloads are selected using argument names, rank, and numeric type; select an
ambiguous overload explicitly with a resolved name such as `f(real,vector)`.
For empty integer arguments, supply an integer-dtype NumPy array (`[]` defaults
to real). This is the native value-only interpreter, not autodiff: complex,
void, RNG, and `_lp` entry points are outside this interface.

From a repository checkout, compare an installed wheel's steady-state calls
with plain Python and NumPy:

```sh
python tools/bench_python_function.py
```

For a development build, stage the Release library in `python/stanli/_bin/`
and prefix the command with `PYTHONPATH=python`.
The benchmark excludes one-time compilation from call latency, checks the
answers first, alternates implementations, and reports medians and IQRs.

Reuse a `Function` handle across calls: its native function lookup tables are
cached at construction. Exact Python `float` and `int` arguments use a direct
scalar path; NumPy scalars and other array-like values retain NumPy conversion.
Overload selection, integer bounds, and shape validation still apply on every
call. See the [optimization measurements and four-way A/B command](../docs/superpowers/plans/2026-08-30-python-function-overhead.md)
for separate measurements of scalar packing and native lookup caching.

## How it works

Every Stan model is a composition of a fixed vocabulary of operations:
densities, constraint transforms, linear algebra, elementwise math.
stanli ships those precompiled and turns each model into *data*, a
static graph of ops over flat preallocated buffers, instead of
generating and compiling C++ per model. The graph doubles as the
autodiff tape, so a reverse sweep is a backwards loop over an array,
and steady-state gradient evaluation allocates nothing.

Two things are not reimplemented, which is what makes the results
trustworthy: the compiler is the real stanc3 plus the stanli OCaml pipeline,
embedded in the runtime on macOS and Linux and packaged as an executable on
Windows, and the math is unmodified stan-math, the same code CmdStan runs.

## Correctness

Nothing here ships on "looks close".

**<!--gen:corpus_verified_of-->118 of 120<!--/gen--> posteriordb models**
are differentially verified against CmdStan: same model, same data, same
evaluation point, comparing the log density and every single gradient
component. **<!--gen:corpus_bitwise-->41<!--/gen--> agree bitwise.** The
worst deviation across the entire corpus is
**<!--gen:corpus_worst-->2.6e-12<!--/gen--> relative**.

The two exceptions are documented rather than hidden. `sir`'s ODE
solution dips about 1e-9 below a declared lower bound at the shared
evaluation point, where CmdStan rejects it too; `kronecker_gp` matches
on the log density and 436 of 438 gradients, differing on the two that
flow through eigenvectors of a nearly degenerate covariance matrix.

Full per-model accuracy table:
[docs/corpus-status.md](https://github.com/seantalts/stanli/blob/main/docs/corpus-status.md)

## Performance

Per-gradient latency against CmdStan, same models, same evaluation
point, both sides `-O3` with FP contraction pinned off:

<!--gen:bench_table_us-->
| model | stanli | CmdStan | speedup |
| --- | ---: | ---: | ---: |
| `gpcm_latent_reg_irt` | 121.1 us | 1337.7 us | **11.0x** |
| `dogs` | 7.1 us | 63.7 us | **8.9x** |
| `radon_pooled` | 45.4 us | 320.9 us | **7.1x** |
| `GLM_Poisson_model` | 0.39 us | 2.0 us | **5.2x** |
| `state_space_stochastic_level_stochastic_seasonal` | 6.7 us | 26.3 us | **3.9x** |
| `eight_schools_noncentered` | 0.23 us | 0.74 us | **3.2x** |
| `logistic_regression_rhs` | 40.8 us | 113.1 us | **2.8x** |
| `normal_mixture` | 41.8 us | 88.2 us | **2.1x** |
| `hierarchical_gp` | 29.9 us | 47.6 us | **1.6x** |
| `hmm_example` | 17.5 us | 27.1 us | **1.6x** |
| `garch11` | 7.0 us | 9.7 us | **1.4x** |
| `diamonds` | 31.1 us | 31.5 us | **1.0x** |
| `one_comp_mm_elim_abs` | 522.9 us | 470.7 us | 0.90x |
| `soil_incubation` | 67.8 us | 60.9 us | 0.90x |
| `lotka_volterra` | 47.6 us | 41.3 us | 0.87x |
<!--/gen-->

The wins come from op granularity. CmdStan's var tape allocates, walks,
and frees one node per scalar operation per leapfrog step; stanli pays
a fixed cost per *op*, and a vectorized statement over N elements
amortizes that to nothing. Across the whole posteriordb corpus the
median is <!--gen:corpus_median-->2.91x<!--/gen--> and
<!--gen:corpus_at_par-->116<!--/gen--> of
<!--gen:corpus_n_grad-->119<!--/gen--> models are at or above CmdStan.

Repeated independent work produces the largest wins. Dense kernels and serial
recurrences land closer to parity because there is less work to fuse, though
the measured HMM, ARMA, and GARCH gradients are now all at least as fast as
CmdStan. The only gradient losses are the three ODE models at 0.87-0.90x;
their first complete runs still win once CmdStan's model build is included.

Method and full table:
[docs/benchmarks.md](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md)

## API

The surface is small on purpose.

```python
import stanli

# A path to a .stan file, or the model source directly.
model = stanli.Model(stan_file="model.stan", data="data.json")
model = stanli.Model(stan_code=src, data={"J": 8, "y": y, "sigma": sigma})

model.n_unconstrained               # length of the unconstrained vector
model.constrained_names             # ['mu', 'tau', 'theta.1', ...]

lp, grad = model.log_prob_grad(q)   # sampling log density and its gradient

fit = model.sample(seed=1, warmup=1000, samples=1000, delta=0.8,
                   refresh=100)
fit["theta.1"]                      # ndarray, chains concatenated
```

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

`data` accepts a path to a JSON file or a dict of Python scalars,
lists, and numpy arrays. `sample` returns every column CmdStan's CSV
would carry (constrained parameters, transformed parameters, generated
quantities, with RNG draws streamed per chain), named the way CmdStan
names them, so `theta` declared as `vector[8]` arrives as `theta.1`
through `theta.8`. Sampler columns (`lp__`, `divergent__`, ...) are
reachable by name too.

## Platforms

Wheels for macOS (arm64 and x86_64), Linux (x86_64 and aarch64,
manylinux_2_28) and Windows (x86_64). The Windows wheel is built under
mingw-w64, because stan-math does not build under MSVC (the same reason
RStan ships through RTools). Windows wheels built from this revision run
`stanli-compile.exe` as a short-lived subprocess and keep pristine `stanc.exe`
beside it for one rollback cycle; there is no OCaml compiler DLL. Python
prefers `stanli-compile.exe` whenever
it is present and selects `stanc.exe` only when it is absent. Launch errors,
compiler errors, and empty output are reported; an invalid portable result is
rejected when decoded. None causes an automatic retry through stock stanc. The
public API works the same way as the embedded compiler path.

The installed library is 29.7 MB: over half of it is the density
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
