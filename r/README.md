# stanli for R

Compile and sample Stan models without a C++ toolchain.

## Install

```r
# prebuilt binaries from R-universe (recommended)
install.packages("stanli", repos = "https://seantalts.r-universe.dev")

# or from source
# install.packages("remotes")
# remotes::install_github("seantalts/stanli@*release", subdir = "r")

# or from a checkout
# R CMD INSTALL r
```

On macOS and Windows, R-universe installs a prebuilt binary. A source install
compiles a small C bridge with the tools that R needs for any source package:
Xcode command line tools on macOS, `r-base-dev` on Debian and Ubuntu, or
Rtools on Windows. The sampler is never compiled on your machine.

Then, once per machine:

```r
stanli_install()
```

This downloads the runtime for your platform (about 10 MB) into
`tools::R_user_dir("stanli", "cache")`. Each package version uses the runtime
from its own release, so run `stanli_install()` again after you upgrade the
package. Nothing else is downloaded. To use a runtime library that you built
yourself, set `STANLI_RUNTIME`.

### Installing from main

To try changes that are not yet in a release, install the package from the
main branch and then the runtime that was built from the same commit:

```r
remotes::install_github("seantalts/stanli", subdir = "r")
stanli::stanli_install("dev")
```

The runtime for a commit appears about an hour after the commit lands on
main, so a commit that was merged in the last hour may not have one yet.
Only the last 10 commits on main keep a runtime, so reinstall the package
from main if yours is older.

Installing the R package from main alone still uses the latest release's
runtime by default. Run `stanli::stanli_install("dev")` as well to try runtime
fixes on main. For example, `multinomial_rng` is supported on main but is
missing from the v0.30.1 runtime. If an error names that function and shows
build `abi6-f63191b99347`, use both installation steps above and restart R
to load the new runtime.

## Quick start

```r
library(stanli)

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)

summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
stanli_diagnose(fit)  # divergences, treedepth, E-BFMI, R-hat, ESS
as_draws_array(fit)   # a posterior::draws_array
```

To pass the model source as a string, use `code =` in place of `file =`.
Sampling four chains of Eight Schools and printing the summary and
diagnostics takes about 70 ms.

stanli looks for `#include` files in the directory that contains `file`. To
add shared directories, pass `include_paths = c("shared", "vendor/stan")`.
stanli searches them in order, then the file's directory (or the current
directory when you use `code`). `sample_cstan()` and `cstan_model()` take
`include_paths` too. The model keeps the included code after it compiles, so
later sampling does not read those files again.

`stanli_model(..., fast_math = TRUE)` turns on
[fast mode](../docs/fast-mode.md). Gradients are faster, and results can
differ from the default in the last bits.

## Sampling and diagnostics

Chains run in parallel by default. Each chain has its own executor and
random number stream, so a parallel run gives exactly the same draws as a
sequential one.

While it samples, stanli prints the progress and elapsed time of each chain.
It also prints notices for rejected proposals, and warnings for divergences,
maximum treedepth or low E-BFMI. Set `refresh = 0` to turn off the progress,
timing and warnings. The notices for rejected proposals print either way, as
in CmdStan; `show_messages = FALSE` drops them, along with anything the model
prints.
Once a chain has post-warmup divergences or maximum-treedepth hits, its
progress lines show the counts so far:

```text
Chain [1] Iteration: 1100 / 2000 [ 55%]  (Sampling)  3 divergent, 12 at max treedepth (10)
```

`fit$report` holds the exact times and problem counts for each chain.
Ctrl-C (or RStudio's stop button) stops every chain after its current
transition.

`summary()` uses Stan's own estimators (rank-normalized split R-hat, bulk and
tail ESS, MCSE), so its output matches `stansummary`. `stanli_diagnose()` runs
the standard checks. One of them is E-BFMI, which detects a heavy tail that
the sampler has explored badly.

`init` is an unconstrained vector, or a matrix with one row per chain.
`unconstrain(m, list(mu = 0.5, sigma = 1.2))` builds it from constrained
values. It names any parameter that is missing, the wrong size, or outside
its support. `init_radius = 0` starts at the origin.

### Metric and step size

`metric` is `"diag_e"` (the default), `"dense_e"` or `"unit_e"`, as in CmdStan.
`inv_metric` sets the starting inverse metric: a vector for `"diag_e"`, a
matrix for `"dense_e"`, or a list with one per chain. `step_size` sets the
initial step size (default 1). `adapt_engaged = FALSE` turns adaptation off,
so the metric and step size stay as given. `adapt_metric = FALSE` keeps
adaptation on for the step size only and leaves `inv_metric` as supplied.
After sampling, `fit$metric`, `fit$step_size` and `fit$inv_metric` hold the
values that each chain adapted. This example reuses them from a pilot run:

```r
pilot <- sample_model(m, seed = 1, metric = "dense_e")
fit <- sample_model(m, seed = 2, metric = "dense_e", warmup = 0,
                    adapt_engaged = FALSE, inv_metric = pilot$inv_metric,
                    step_size = mean(pilot$step_size))
```

With fewer than 150 warmup iterations, Stan does not adapt the metric and
keeps any metric that you supplied. `warmup = 0` needs `adapt_engaged =
FALSE`, as in the example, and then sampling runs at exactly `step_size`.
With adaptation left on, `warmup = 0` is an error, as in CmdStan and
cmdstanr.

`sample_more(fit, samples = 1000)` continues each chain from where it
stopped. Each chain keeps its step size, metric and random stream, and there
is no new warmup. The function returns a new fit with the new draws appended.
The draws are the same, bit for bit, as asking for all of them in the first
call:

```r
fit <- sample_model(m, seed = 1, samples = 1000)
fit <- sample_more(fit, samples = 1000)   # 2000 per chain
```

### Within-chain parallelism

To run eligible `reduce_sum` calls on worker threads, build the model with
`stanli_model(..., threads_per_chain = 4)`, or pass `threads_per_chain = 4`
to `sample_cstan()` or `cstan_model(code)$sample()`, which build the model
themselves. Sampling, optimization, the approximate methods and direct
gradients then use that count; `sample_nutpie()` does not use it. Small or
unsupported calls run serially. To use another count, build a new model. The
rounding can change with the count, and so can the draws. Passing
`threads_per_chain` to `sample_model()` or `sample_more()` is deprecated.
`fit$model$reduce_sum_count` and `fit$model$reduce_sum_fallbacks` report what
was kept. See
[native reductions](../docs/native-reduce-sum.md).

### Workflow packages

`bayesplot`, `loo`, `tidybayes` and `posterior` are optional. Their functions
work on a fit directly, for example `bayesplot::nuts_params(fit)`,
`bayesplot::log_posterior(fit)`, `bayesplot::rhat(fit)`,
`bayesplot::neff_ratio(fit)` and `tidybayes::spread_draws(fit, mu, theta[j])`.
`loo::loo(fit)` works when the model declares pointwise `log_lik` in
generated quantities.

stanli's `neff_ratio()` uses bulk ESS, while bayesplot's CmdStanMCMC method
uses basic ESS. `summary(fit)` prints the stansummary table. For cmdstanr's
default table, use `posterior::summarise_draws(as_draws_array(fit))`.
`as_draws_array()` and the methods for these packages leave out saved warmup
unless you pass `inc_warmup = TRUE`.

[Coming from cmdstanr](../docs/from-cmdstanr.md) translates the API call by
call. [Teaching with stanli](../docs/teaching.md) covers classroom setup,
offline labs, and models generated by brms and Rethinking.

## Other algorithms

stanli runs Stan's own implementations of these algorithms on the same
model:

```r
vi <- variational_model(m, algorithm = "meanfield")  # or "fullrank": ADVI
pf <- pathfinder_model(m)                            # single-path Pathfinder
la <- laplace_model(m)                               # normal at the posterior mode
op <- optimize_model(m)                              # L-BFGS posterior mode
gq <- sample_model(m, fixed_param = TRUE)            # only generated quantities move
fit <- sample_model(m, pathfinder_init = list())     # NUTS started from Pathfinder
```

The three approximations return a `stanli_fit` with one chain of independent
draws, so `as_draws_array()`, `summary()` and `as_stanfit()` work. Their
`fit$sampler` holds `lp__` and `lp_approx__` (the log density of the model
and of the approximation). `stanli_diagnose()` and `nuts_params()` need a NUTS
fit. Argument names follow cmdstanr. If Stan reports a problem with a run,
such as ADVI stopping at its iteration limit, stanli repeats it as a warning.

stanli does not check any of the approximations against the posterior. When
the posterior is far from normal on the unconstrained scale, they can be
badly wrong, more often in the spread than in the centre. Compare with
`sample_model()` before you rely on one.

`optimize_model()` returns the mode. Pass its `$unconstrained` element to
`sample_model(init = )`. `pathfinder_init` accepts `num_iterations`,
`num_elbo_draws`, `history_size` and `init_radius`. You cannot combine it
with `init`, and it does no PSIS resampling.

## Laplace whitening

`laplace_whiten(m, latent = "a")` is experimental. It finds the Laplace mode
of the hyperparameters. It returns `$model`, a model whose random effects are
whitened around that mode. Sample that model with `metric = "unit_e"` and
`adapt_metric = FALSE`. `laplace_whiten()` needs the Matrix package. See
[Laplace whitening](../docs/laplace-whitening.md).

## nutpie

`sample_nutpie()` samples the model with the
[nutpie](https://github.com/pymc-devs/nutpie) sampler. nutpie calls the model
through the runtime's BridgeStan interface. You need the nutpieR package,
version 1.9.0 or later, which is not on CRAN:

```r
install.packages("nutpieR", repos = c("https://andytimm.r-universe.dev",
                                      "https://cloud.r-project.org"))
fit <- sample_nutpie(m, chains = 4, seed = 1, adaptation = "low_rank")
```

The result is a `stanli_fit`, so `summary()`, `stanli_diagnose()`,
`as_draws_array()`, `log_lik()` and the fit adapters work on it. Its `lp__`
is the same quantity as in `sample_model()`. `fit$inv_metric` is `NULL`,
because `sample_model(inv_metric = )` cannot take nutpie's mass matrix. To
pass other `nutpieR::nutpie_sample()` arguments, such as `target_accept` and
`max_treedepth`, use `...`.

To drive any other BridgeStan client, load `stanli_runtime_path()` as the
model library and pass `bridgestan_data(m)` as the data argument.

## WALNUTS

`sample_walnuts()` samples the model with WALNUTS
([arXiv:2506.18746](https://arxiv.org/abs/2506.18746)), a version of NUTS
that halves its step size inside a trajectory where the posterior is hard,
such as the neck of a funnel. It is built into the runtime, so it needs no
other package:

```r
fit <- sample_walnuts(m, chains = 4, seed = 1, max_error = 0.5)
fit <- sample_walnuts(m, pathfinder_init = list())  # Pathfinder starting points
```

The chains run one after another. Seeds, chain ids and starting values work
as in `sample_model()`. Without `init` or `pathfinder_init`, chain 1 starts
where NUTS chain 1 with the same seed starts. `pathfinder_init` accepts the
same options as in `sample_model()` and cannot be combined with `init`. It runs one Pathfinder path, keyed on the
first chain id, to generate all chains' starts; splitting a run into separate
calls can therefore change those starts. The result is a `stanli_fit`, so
`summary()`, `as_draws_array()`, `log_lik()` and the fit adapters work on it.
`fit$sampler` holds `lp__` and
`stepsize__`: the runtime does not report divergences, tree depths or the
adapted metric for WALNUTS. For the same reason, `sample_more()`,
`stanli_diagnose()` and `nuts_params()` refuse the fit. There is no thinning
and no saved warmup.

## Fit adapters

If you have code written for RStan or CmdStanR, three adapters turn a fit
into the kind of object that the code expects:

| Adapter | Returns | Needs | Limit |
| --- | --- | --- | --- |
| `as_stanfit(fit)` | An S4 subclass of `rstan::stanfit` with live density, gradient and transform methods | RStan, loaded on conversion only | Passes `inherits(x, "stanfit")`; exact class-equality checks fail unless `plain = TRUE`, and RStan's native resampling does not work |
| `as_rfit(fit)` | An S3 object with RStan-style `extract()`, `summary()$summary`, `get_sampler_params()` and `get_elapsed_time()` | stanli, plus posterior for summaries | Not an RStan object: code calling `rstan::extract()` must call `stanli::extract()` |
| `as_cstanfit(fit)`, `sample_cstan()`, `cstan_model(code)$sample()` | An object with CmdStanR-style `$draws()`, `$summary()`, `$sampler_diagnostics()`, `$metadata()`, `$inv_metric()`, `$num_chains()`, `$time()` and `$loo()` | stanli, plus posterior and loo for those methods | Its own `stanli_cstanfit` class; no CSV, executable or `$output()` methods, no LOO moment matching |

### as_stanfit

`as_stanfit()` builds the object in memory from stored arrays, with no CSV
files and no C++ compilation. It keeps the model, so `rstan::log_prob()`,
`grad_log_prob()`, `constrain_pars()`, `unconstrain_pars()` and
`get_num_upars()` work:

```r
sf <- as_stanfit(fit)
rstan::extract(sf)
q <- fit$unconstrained[1, 1, ]
rstan::log_prob(sf, q, gradient = TRUE)
rstan::unconstrain_pars(sf, rstan::constrain_pars(sf, q))
```

Extraction, summaries, plotting, ordinary LOO, ShinyStan, and
`loo::loo_moment_match(sf, loo = loo::loo(sf), cores = 1)` work. The density
methods always include the Jacobian of the parameter transforms, so
`adjust_transform = FALSE` is an error. When `constrain_pars()` computes
random generated quantities, it advances the random number generator of the
model that the fit keeps. Initial values and adaptation text are empty.
Declarations with zero elements cannot be recovered from column names.

A fit from `variational_model()`, `pathfinder_model()` or `laplace_model()`
converts to a one-chain `stanfit` with no warmup, no sampler statistics, and
`lp__` and `lp_approx__` as its last columns. A fixed-parameter fit converts
with algorithm `"Fixed_param"`.

CRAN has RStan binaries for supported macOS and Windows R versions; install
with `install.packages("rstan", type = "binary")` to avoid a C++ toolchain.
Linux classrooms should set up a binary install. In a fresh session, load
RStan before rethinking. Otherwise rethinking's placeholder `stanfit` class
causes problems.

`saveRDS()` keeps the draws but not the live model. After `readRDS()`, recreate
the model from the same source and data and attach it:

```r
saved <- readRDS("fit.rds")
m <- stanli_model(file = "model.stan", data = original_data)
sf <- as_stanfit(saved, model = m)  # a saved stanli_fit or stanli_stanfit
```

Without a model, extraction and summaries still work, and density calls say
how to attach one. To make permuted extraction reproducible, set R's seed
before the conversion.

### as_rfit

```r
native <- as_rfit(fit)
post <- stanli::extract(native)       # named, draw-first parameter arrays
summary(native)$summary               # RStan-shaped summary matrix
stanli::get_sampler_params(native)    # one matrix per chain
stanli::get_elapsed_time(native)      # chain-by-phase seconds
```

`as_rfit()` never loads RStan. It needs no runtime for saved draws. The
result keeps class `stanli_fit`, so the bayesplot, loo and tidybayes methods
still work. `n_eff` and `se_mean` use posterior's basic ESS, so they can
differ from RStan's. Permuted extraction leaves out warmup and keeps joint
draws together. To reproduce it, set R's seed before `as_rfit()`.

### as_cstanfit, sample_cstan and cstan_model

```r
fit <- sample_cstan(
  model_code = "parameters { real mu; } model { mu ~ normal(0, 1); }",
  chains = 4, parallel_chains = 4, iter_warmup = 500, iter_sampling = 1000,
  init = list(mu = 0), seed = 42, refresh = 0
)
fit$draws("mu")
fit$summary("mu", "mean", "sd", "rhat", "ess_bulk")
```

`sample_cstan()` takes CmdStanR's sampler arguments, including `metric`,
`inv_metric`, `step_size`, `adapt_engaged` and `threads_per_chain`, and
returns the same object as `as_cstanfit()`. Unknown options are an error.
`init` can be a constrained list, a list with one list per chain, or a
function that returns a list. Parameters that you leave out start at random
values. `cstan_model(code)$sample(...)` takes the same arguments, so you can
reuse a CmdStanR sampling call by changing only the constructor.

Summaries use posterior's rank-normalized R-hat and bulk/tail ESS, as
CmdStanR does. `$metadata()` gives iteration counts before thinning.
Diagnostic columns are in alphabetical order, so select them by name. stanli
records the time for each chain but not the overall wall time.
`$stanli_fit()` returns the original fit.

## rstan-style code

`as_stanmodel()` wraps Stan code so that `rstan::sampling()` and `rstan::vb()`
run on stanli, with no C++ compilation:

```r
m <- as_stanmodel(code)                      # or a .stan path, or an rstan stanmodel
fit <- rstan::sampling(m, data = d, chains = 4, iter = 2000, seed = 1)
rstan::extract(fit); print(fit); rstan::log_prob(fit, q)
```

The result is a `stanli_stanfit`. stanli does not change anything in
rstan's namespace. This works for plain rstan workflows. It also works for
packages that call `sampling()` on a model that you supply, such as a
function that takes a `stanmodel` argument.

`iter`, `warmup`, `thin`, `chains`, `seed`, `init`, `pars`, `include`, `cores`,
`control` and `algorithm = "Fixed_param"` follow rstan. `control` takes
`adapt_delta`, `max_treedepth`, `stepsize`, `metric` and `adapt_engaged`. Any
other entry gives an error that names it. `algorithm = "HMC"`, `sample_file`
and `diagnostic_file` also give errors.

`init` takes every rstan form: `"random"`, a radius, `"0"`, a list, a list of
lists with one per chain, or a function with or without a `chain_id`
argument. Parameters that a list leaves out start at random points, and
names that are not parameters are ignored. If a start has a non-finite log
density or gradient, stanli draws a new one, up to 100 times. As in rstan,
the default `seed` draw advances your R random seed, and nothing else does.

What does not work:

- Models compiled ahead of time inside other packages, such as rstanarm, RBesT,
  OncoBayes2 and prophet. They call their own compiled models and never call
  `sampling()` on Stan code.
- `rstan::optimizing()`. stanli's log density always includes the
  change-of-variables Jacobian, so the result would differ from rstan's mode.
  The call gives an error that points you to `optimize_model()`.
- `vb()` with `importance_resampling = TRUE` or `keep_every` other than 1.

`as_stanfit(fit, plain = TRUE)` returns an object of class exactly `stanfit`
for code that checks `class(x) == "stanfit"`. It keeps the draws and drops the
model, so `log_prob()` and the other model methods do not work on it.
`as_stanfit(plain_fit, model = m)` attaches a model again.

## API reference

```r
stanli_install(); stanli_available(); stanli_runtime_path()

m <- stanli_model(file = NULL, code = NULL, data = NULL, mir = NULL, seed = 1,
                  threads_per_chain = 1, include_paths = NULL, fast_math = FALSE,
                  allow_interpreter = TRUE)
stanli_check_syntax(code)
log_prob_grad(m, q)                 # log density and gradient
log_prob_hessian(m, q)              # and the Hessian at q
unconstrain(m, values)              # constrained list -> unconstrained vector

fit <- sample_model(m, chains = 4, seed = 1, warmup = 1000, samples = 1000,
                    thin = 1, delta = 0.8, max_depth = 10, save_warmup = FALSE,
                    init = NULL, init_radius = 2, pathfinder_init = NULL,
                    parallel_chains = NULL, refresh = 100,
                    fixed_param = FALSE,
                    metric = "diag_e", inv_metric = NULL, step_size = NULL,
                    adapt_engaged = NULL, adapt_metric = TRUE,
                    show_messages = TRUE)
optimize_model(m, seed = 1, iter = 2000, init = NULL, init_radius = 2)
variational_model(m, algorithm = "meanfield", iter = 10000, draws = 1000, ...)
pathfinder_model(m, draws = 1000, ...)
laplace_model(m, mode = NULL, draws = 1000, ...)
sample_nutpie(m, chains = 4, seed = 1, warmup = NULL, samples = 1000,
              adaptation = "diag", ...)   # needs nutpieR
sample_walnuts(m, chains = 4, seed = 1, warmup = 1000, samples = 1000,
               max_error = 0.5, init = NULL, init_radius = 2, chain_id = 1,
               refresh = 100, pathfinder_init = NULL)
laplace_whiten(m, latent = NULL, init = NULL, seed = 1, ...)  # experimental, needs Matrix
whiten(m, q_hat, perm, L, lowrank = NULL); parameter_annotations(m)
bridgestan_data(m); stanli_build_id()

summary(fit); stanli_diagnose(fit); log_lik(fit)
as_draws_array(fit, include_sampler = FALSE, inc_warmup = FALSE)
as_stanfit(fit, plain = FALSE); as_rfit(fit); as_cstanfit(fit)
as_stanmodel(code, model_name = NULL)   # rstan::sampling() and vb() on stanli
sample_cstan(model_code, data, ...); cstan_model(model_code)
```

Each function's help page (`?sample_model`) lists every argument.

## Platforms

R-universe has binaries for macOS, Windows, and Ubuntu Linux on x86_64 and
arm64; see [Teaching](../docs/teaching.md#linux-labs) for the Linux repository
URL. The runtime is a separate shared library of about 30 MB. It contains
stan-math, the density kernels and the interpreter. The package and the
runtime each carry a layout version. If the versions do not match, stanli
stops with a message that says which one to update.

The macOS and Linux runtimes include the Stan compiler. On Windows the
compiler is a separate short-lived program, `stanli-compile.exe`, in the same
folder as the runtime. If neither is available,
R uses the bundled JavaScript build of the same compiler through V8. webR
uses that build too.

## Limits

- `optimize_model()` and `laplace_model()` always include the Jacobian.
  CmdStan's `optimize` default, `jacobian = 0` (penalized maximum likelihood),
  is not available because the Jacobian is part of the compiled graph.
- `pathfinder_model()` runs a single path. Multi-path Pathfinder, cmdstanr's
  default, is not available: Stan 2.40's multi-path service reads past the end
  of an array while it resamples. `num_paths` above 1 is an error, and a
  single path has no importance resampling or Pareto k.
- Standalone generated quantities over an earlier fit's draws are not
  available.
- Draws are kept in memory. There are no per-chain CSV files.

How stanli works is in [docs/how-it-works.md](../docs/how-it-works.md), how it
is tested in [TESTING.md](../TESTING.md), and how fast it is in
[docs/benchmarks.md](../docs/benchmarks.md).
