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
eight megabytes. Model preparation takes milliseconds, so the first
draw arrives about 20x sooner than a toolchain that compiles C++ per
model.

```python
import stanli

model = stanli.Model(stan_file="eight_schools.stan", data="data.json")
fit = model.sample(seed=1, chains=4, warmup=1000, samples=1000)

fit["mu"].mean()        # every draw of a column, chains concatenated
fit.draws("mu")         # (chains, draws), for a trace plot
```

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

## How it works

Every Stan model is a composition of a fixed vocabulary of operations:
densities, constraint transforms, linear algebra, elementwise math.
stanli ships those precompiled and turns each model into *data*, a
static graph of ops over flat preallocated buffers, instead of
generating and compiling C++ per model. The graph doubles as the
autodiff tape, so a reverse sweep is a backwards loop over an array,
and steady-state gradient evaluation allocates nothing.

Two things are not reimplemented, which is what makes the results
trustworthy: the compiler is the real stanc3, linked in-process, and
the math is unmodified stan-math, the same code CmdStan runs.

## Correctness

Nothing here ships on "looks close".

**<!--gen:corpus_verified_of-->118 of 120<!--/gen--> posteriordb models**
are differentially verified against CmdStan: same model, same data, same
evaluation point, comparing the log density and every single gradient
component. **<!--gen:corpus_bitwise-->45<!--/gen--> agree bitwise.** The
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
| model | params | stanli | CmdStan | speedup |
| --- | ---: | ---: | ---: | ---: |
| `radon_pooled` | 3 | 52.9 us | 320.9 us | **6.1x** |
| `arK` | 7 | 2.4 us | 12.5 us | **5.2x** |
| `radon_hierarchical_intercept_centered` | 391 | 111.6 us | 569.1 us | **5.1x** |
| `radon_county_intercept` | 388 | 89.7 us | 431.6 us | **4.8x** |
| `nes` | 10 | 19.7 us | 69.3 us | **3.5x** |
| `eight_schools_noncentered` | 10 | 0.23 us | 0.74 us | **3.3x** |
| `election88_full` | 90 | 295.3 us | 902.0 us | **3.0x** |
| `bym2_offset_only` | 3845 | 39.6 us | 114.6 us | **2.9x** |
| `dogs` | 3 | 22.0 us | 63.7 us | **2.9x** |
| `kidscore_momiq` | 3 | 1.9 us | 4.9 us | **2.6x** |
| `lsat_model` | 1006 | 45.5 us | 91.2 us | **2.0x** |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 17.2 us | 26.3 us | **1.5x** |
| `hmm_example` | 4 | 21.1 us | 27.1 us | **1.3x** |
| `garch11` | 4 | 8.2 us | 9.7 us | **1.2x** |
| `hmm_drive_0` | 6 | 117.6 us | 132.8 us | **1.1x** |
| `normal_mixture` | 3 | 79.0 us | 88.2 us | **1.1x** |
| `low_dim_gauss_mix` | 5 | 88.9 us | 98.3 us | **1.1x** |
| `wells_dist100ars_model` | 3 | 17.4 us | 19.0 us | **1.1x** |
| `iohmm_reg` | 29 | 301.4 us | 320.3 us | **1.1x** |
| `radon_county` | 389 | 83.2 us | 82.1 us | **1.0x** |
| `arma11` | 4 | 6.7 us | 6.2 us | 0.92x |
| `diamonds` | 26 | 35.4 us | 31.5 us | 0.89x |
| `ldaK2` | 7 | 145.9 us | 104.1 us | 0.71x |
<!--/gen-->

The wins come from op granularity. CmdStan's var tape allocates, walks,
and frees one node per scalar operation per leapfrog step; stanli pays
a fixed cost per *op*, and a vectorized statement over N elements
amortizes that to nothing. Across the whole posteriordb corpus the
median is <!--gen:corpus_median-->2.07x<!--/gen--> and
<!--gen:corpus_at_par-->100<!--/gen--> of
<!--gen:corpus_n_grad-->119<!--/gen--> models are at or above CmdStan.

The former worst class -- recurrences -- crossed parity when the
runtime started compiling them: `hmm_*`, `garch11` and `iohmm_reg`
step through time with each step reading the last one's
parameter-dependent result, which nothing can vectorize, so each model
now compiles its recurrence into a register program with a generated
derivative program alongside. The remaining losses are mixtures with
more than two components (a reduction shape the fusion pass does not
yet express) and ODE models at around 0.6x, whose right-hand side runs
through the register machine where CmdStan runs native code.

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

fit = model.sample(seed=1, warmup=1000, samples=1000, delta=0.8)
fit["theta.1"]                      # ndarray, chains concatenated
```

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
RStan ships through RTools), and bundles `stanc.exe` as a subprocess
instead of embedding the compiler; the API works the same way either
way.

The installed library is 22.2 MB: over half of it is the density
kernels, about a quarter the embedded stanc3, and the interpreter and
NUTS together are about 410 KB. That is the trade this design makes:
ship the compiler and every kernel once, so nothing is ever built on
the user's machine.

## Limits

Stated plainly:

- The sampler is Stan's own NUTS with diagonal-metric adaptation, and
  `optimize()` is Stan's L-BFGS. No variational inference or Pathfinder
  yet.
- `inits` are on the unconstrained scale. Constrained inits would need
  the inverse parameter transforms, which do not exist here yet.
- `optimize(jacobian=False)` (CmdStan's default penalized maximum
  likelihood) raises; see above.

What is here is verified against CmdStan model by model, and every
number on this page is reproducible from the repository.

- Source, issues, and roadmap:
  [github.com/seantalts/stanli](https://github.com/seantalts/stanli)
- License: BSD-3-Clause, matching Stan's own.
