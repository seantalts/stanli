# stanli for R

Compile and sample Stan models without a C++ toolchain.

## Install

```r
# prebuilt binaries from R-universe (recommended)
install.packages("stanli", repos = "https://seantalts.r-universe.dev")

# or from source
# install.packages("remotes")
# remotes::install_github("seantalts/stanli", subdir = "r")

# or from a checkout
# R CMD INSTALL r
```

On macOS and Windows R-universe installs a prebuilt binary. A source install
builds a small C bridge with the toolchain R already expects for source
packages (Xcode command line tools on macOS, `r-base-dev` on Debian and
Ubuntu, Rtools on Windows). The sampler itself is never compiled on your
machine.

Then, once per machine:

```r
stanli_install()
```

This downloads the runtime for your platform, about 10 MB, into
`tools::R_user_dir("stanli", "cache")`. The runtime is pinned to the release
the package was built against, so after a package upgrade this step runs once
more. Nothing else is downloaded. Set `STANLI_RUNTIME` to use a library you
built yourself.

## Quick start

```r
library(stanli)

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)

summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
stanli_diagnose(fit)  # divergences, treedepth, E-BFMI, R-hat, ESS
as_draws_array(fit)   # a posterior::draws_array
```

`code =` takes the model source as a string. Four chains of eight schools,
with summary and diagnostics, take about 70 ms.

`#include` directives search the directory containing `file`. Add shared
directories with `include_paths = c("shared", "vendor/stan")`; they are
searched in order, then the file's directory (the current directory for
`code`). `sample_cstan()` and `cstan_model()` take `include_paths` too. A
model keeps its compiled includes, so later sampling does not reread them.

`stanli_model(..., fast_math = TRUE)` opts in to
[fast mode](../docs/fast-mode.md): faster gradients, with results that can
differ from the default in the last bits.

## Sampling and diagnostics

Chains run in parallel by default. Each chain owns its executor and RNG
stream, so a parallel run is byte-identical to a sequential one. Sampling
prints per-chain progress, elapsed times, notices for rejected proposals, and
any divergence, maximum-treedepth or low E-BFMI warnings; `refresh = 0`
silences it. Exact per-chain times
and problem counts are in `fit$report`. Ctrl-C (or RStudio's stop button)
stops every chain after its current transition.

`summary()` uses Stan's own estimators (rank-normalized split R-hat, bulk and
tail ESS, MCSE), so it matches `stansummary`. `stanli_diagnose()` runs the
standard checks, including E-BFMI, which catches a badly explored heavy tail.

`init` is an unconstrained vector, or a matrix with one row per chain.
`unconstrain(m, list(mu = 0.5, sigma = 1.2))` builds it from constrained
values and names any parameter that is missing, the wrong size, or outside its
support. `init_radius = 0` starts at the origin.

### Metric and step size

`metric` is `"diag_e"` (the default), `"dense_e"` or `"unit_e"`, as in CmdStan.
`inv_metric` sets the starting inverse metric: a vector for `"diag_e"`, a
matrix for `"dense_e"`, or a list with one per chain. `step_size` sets the
initial step size (default 1). `adapt_engaged = FALSE` turns adaptation off,
so the metric and step size stay as given. After sampling, `fit$metric`,
`fit$step_size` and `fit$inv_metric` hold what each chain adapted:

```r
pilot <- sample_model(m, seed = 1, metric = "dense_e")
fit <- sample_model(m, seed = 2, metric = "dense_e", warmup = 0,
                    adapt_engaged = FALSE, inv_metric = pilot$inv_metric,
                    step_size = mean(pilot$step_size))
```

With fewer than 150 warmup iterations Stan does not adapt the metric, and a
supplied one is kept. With `warmup = 0` and adaptation on, Stan still runs its
initial step size search, so use `adapt_engaged = FALSE` to sample at exactly
`step_size`.

### Within-chain parallelism

`threads_per_chain = 4` in `sample_model()`, `sample_cstan()` or
`cstan_model(code)$sample()` runs eligible `reduce_sum` calls on worker
threads. Small or unsupported calls stay serial. Changing the thread count
prepares a new model and can change rounding and therefore the draws.
`fit$model$reduce_sum_count` and `fit$model$reduce_sum_fallbacks` report what
was kept. See [native reductions](../docs/native-reduce-sum.md).

### Workflow packages

`bayesplot`, `loo`, `tidybayes` and `posterior` are optional and work on a fit
directly: `bayesplot::nuts_params(fit)`, `bayesplot::log_posterior(fit)`,
`bayesplot::rhat(fit)`, `bayesplot::neff_ratio(fit)`,
`tidybayes::spread_draws(fit, mu, theta[j])`, and `loo::loo(fit)` when the
model declares pointwise `log_lik` in generated quantities. `neff_ratio()`
uses bulk ESS; bayesplot's CmdStanMCMC method uses basic ESS. `summary(fit)`
prints the stansummary table; for cmdstanr's default table use
`posterior::summarise_draws(as_draws_array(fit))`. Saved warmup is left out
of `as_draws_array()` and the ecosystem methods unless `inc_warmup = TRUE`.

[Coming from cmdstanr](../docs/from-cmdstanr.md) translates the API call by
call. [Teaching with stanli](../docs/teaching.md) covers classroom setup,
offline labs, and models generated by brms and Rethinking.

## Other algorithms

These are Stan's own implementations, run on the same model:

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
`fit$sampler` holds `lp__` and `lp_approx__` (the log density and the
approximation's). `stanli_diagnose()` and `nuts_params()` need a NUTS fit.
Argument names follow cmdstanr. A warning repeats what Stan reports about a
run, such as ADVI stopping at its iteration limit.

None of the approximations checks itself against the posterior. When the
posterior is far from normal on the unconstrained scale they can be badly
wrong, more often in the spread than in the centre. Compare with
`sample_model()` before relying on one.

`optimize_model()` returns the mode; its `$unconstrained` element is what
`sample_model(init = )` takes. `pathfinder_init` accepts `num_iterations`,
`num_elbo_draws`, `history_size` and `init_radius`, cannot be combined with
`init`, and does no PSIS resampling.

## Fit adapters

Three views of a fit serve code written against RStan or CmdStanR:

| Adapter | Returns | Needs | Limit |
| --- | --- | --- | --- |
| `as_stanfit(fit)` | An S4 subclass of `rstan::stanfit` with live density, gradient and transform methods | RStan, loaded on conversion only | Passes `inherits(x, "stanfit")`; exact class-equality checks fail unless `plain = TRUE`, and RStan's native resampling does not work |
| `as_rfit(fit)` | An S3 object with RStan-style `extract()`, `summary()$summary`, `get_sampler_params()` and `get_elapsed_time()` | stanli, plus posterior for summaries | Not an RStan object: code calling `rstan::extract()` must call `stanli::extract()` |
| `as_cstanfit(fit)`, `sample_cstan()`, `cstan_model(code)$sample()` | An object with CmdStanR-style `$draws()`, `$summary()`, `$sampler_diagnostics()`, `$metadata()`, `$inv_metric()`, `$num_chains()`, `$time()` and `$loo()` | stanli, plus posterior and loo for those methods | Its own `stanli_cstanfit` class; no CSV, executable or `$output()` methods, no LOO moment matching |

### as_stanfit

`as_stanfit()` builds the object in memory from stored arrays, with no CSV and
no C++ compilation, and keeps the model for `rstan::log_prob()`,
`grad_log_prob()`, `constrain_pars()`, `unconstrain_pars()` and
`get_num_upars()`:

```r
sf <- as_stanfit(fit)
rstan::extract(sf)
q <- fit$unconstrained[1, 1, ]
rstan::log_prob(sf, q, gradient = TRUE)
rstan::unconstrain_pars(sf, rstan::constrain_pars(sf, q))
```

Extraction, summaries, plotting, ordinary LOO, ShinyStan, and
`loo::loo_moment_match(sf, loo = loo::loo(sf), cores = 1)` work. Density
methods always include the parameter-transform Jacobian, so
`adjust_transform = FALSE` is an error. Random generated quantities in
`constrain_pars()` advance the retained model's RNG. Initial values and
adaptation text are empty, and declarations with zero elements cannot be
recovered from column names.

A fit from `variational_model()`, `pathfinder_model()` or `laplace_model()`
converts to a one-chain `stanfit` with no warmup, no sampler statistics, and
`lp__` and `lp_approx__` as its last columns. A fixed-parameter fit converts
with algorithm `"Fixed_param"`.

CRAN has RStan binaries for supported macOS and Windows R versions; install
with `install.packages("rstan", type = "binary")` to avoid a C++ toolchain.
Linux classrooms should provision a binary install. Load RStan before
rethinking in a fresh session, or rethinking's placeholder `stanfit` class
gets in the way.

`saveRDS()` keeps the draws but not the live model. After `readRDS()`, recreate
the model from the same source and data and attach it:

```r
saved <- readRDS("fit.rds")
m <- stanli_model(file = "model.stan", data = original_data)
sf <- as_stanfit(saved, model = m)  # a saved stanli_fit or stanli_stanfit
```

Without a model, extraction and summaries still work and density calls say how
to attach one. Set R's seed before conversion for reproducible permuted
extraction.

### as_rfit

```r
native <- as_rfit(fit)
post <- stanli::extract(native)       # named, draw-first parameter arrays
summary(native)$summary               # RStan-shaped summary matrix
stanli::get_sampler_params(native)    # one matrix per chain
stanli::get_elapsed_time(native)      # chain-by-phase seconds
```

`as_rfit()` never loads RStan and needs no runtime for saved draws. The result
keeps class `stanli_fit`, so the bayesplot, loo and tidybayes methods still
apply. `n_eff` and `se_mean` use posterior's basic ESS and need not match
RStan's. Permuted extraction leaves out warmup and keeps joint draws together;
set R's seed before `as_rfit()` to reproduce it.

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
returns `as_cstanfit()`. Unknown options are an error. `init` is a constrained
list, one list per chain, or a function returning a list; parameters left out
start at random. `cstan_model(code)$sample(...)` takes the same arguments, so a
CmdStanR sampling call can be reused by changing the constructor.

Summaries use posterior's rank-normalized R-hat and bulk/tail ESS, matching
CmdStanR. `$metadata()` iteration counts are before thinning. Diagnostic
columns are in alphabetical order, so select them by name. Overall wall time
is not recorded, only per-chain times. `$stanli_fit()` returns the original
fit.

## rstan-style code

`as_stanmodel()` wraps Stan code so that `rstan::sampling()` and `rstan::vb()`
run on stanli, with no C++ compilation:

```r
m <- as_stanmodel(code)                      # or a .stan path, or an rstan stanmodel
fit <- rstan::sampling(m, data = d, chains = 4, iter = 2000, seed = 1)
rstan::extract(fit); print(fit); rstan::log_prob(fit, q)
```

The result is a `stanli_stanfit`, and stanli does not change anything in
rstan's namespace. This works for plain rstan workflows, and for packages that
call `sampling()` on a model you can supply, such as a function taking a
`stanmodel` argument.

`iter`, `warmup`, `thin`, `chains`, `seed`, `init`, `pars`, `include`, `cores`,
`control` and `algorithm = "Fixed_param"` follow rstan. `control` takes
`adapt_delta`, `max_treedepth`, `stepsize`, `metric` and `adapt_engaged`; any
other entry is an error that names it, as are `algorithm = "HMC"`,
`sample_file` and `diagnostic_file`. `init` takes every rstan form: `"random"`,
a radius, `"0"`, a list, a list of lists with one per chain, or a function with
or without a `chain_id` argument. Parameters a list leaves out start at random
points, and names that are not parameters are ignored. A start with a
non-finite log density or gradient is redrawn up to 100 times. Your random
seed is advanced by the default `seed` draw, as in rstan, and nothing else.

What does not work:

- Models compiled ahead of time inside other packages, such as rstanarm, RBesT,
  OncoBayes2 and prophet. They call into their own compiled models, not
  `sampling()` on code.
- `rstan::optimizing()`. stanli's log density always includes the
  change-of-variables Jacobian, so it would not return rstan's mode. The call
  is an error that points to `optimize_model()`.
- `vb()` with `importance_resampling = TRUE` or `keep_every` other than 1.

`as_stanfit(fit, plain = TRUE)` returns an object of class exactly `stanfit`
for code that checks `class(x) == "stanfit"`. It keeps the draws and drops the
model, so `log_prob()` and the other model methods stop working;
`as_stanfit(plain_fit, model = m)` attaches a model again.

## API reference

```r
stanli_install(); stanli_available(); stanli_runtime_path()

m <- stanli_model(file = NULL, code = NULL, data = NULL, mir = NULL, seed = 1,
                  threads_per_chain = 1, include_paths = NULL, fast_math = FALSE)
stanli_check_syntax(code)
log_prob_grad(m, q)                 # log density and gradient
unconstrain(m, values)              # constrained list -> unconstrained vector

fit <- sample_model(m, chains = 4, seed = 1, warmup = 1000, samples = 1000,
                    thin = 1, delta = 0.8, max_depth = 10, save_warmup = FALSE,
                    init = NULL, init_radius = 2, pathfinder_init = NULL,
                    parallel_chains = NULL, refresh = 100,
                    threads_per_chain = 1, fixed_param = FALSE,
                    metric = "diag_e", inv_metric = NULL, step_size = NULL,
                    adapt_engaged = TRUE)
optimize_model(m, seed = 1, iter = 2000, init = NULL, init_radius = 2)
variational_model(m, algorithm = "meanfield", iter = 10000, draws = 1000, ...)
pathfinder_model(m, draws = 1000, ...)
laplace_model(m, mode = NULL, draws = 1000, ...)

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
URL. The runtime is a separate shared library of about 30 MB holding
stan-math, the density kernels and the interpreter. Package and runtime carry
a layout version, and loading refuses a mismatch with a message saying which
side to update.

The macOS and Linux runtimes embed the Stan compiler. On Windows it runs as a
short-lived `stanli-compile.exe` subprocess beside the runtime. Without either,
R falls back to stock `stanc` beside the runtime or on `PATH`, and finally to the bundled JavaScript compiler through V8,
which is also what webR uses.

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
- Draws live in memory; there are no per-chain CSV files.

How stanli works is in [docs/how-it-works.md](../docs/how-it-works.md), how it
is tested in [TESTING.md](../TESTING.md), and how fast it is in
[docs/benchmarks.md](../docs/benchmarks.md).
