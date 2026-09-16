# stanli for R

Compile and sample Stan models without a C++ toolchain.

## Install

Install from R-universe or GitHub; the package lives in the `r/` subdirectory
of the repository:

```r
# prebuilt binaries from R-universe (recommended)
install.packages("stanli", repos = "https://seantalts.r-universe.dev")

# or from source
# install.packages("remotes")
# remotes::install_github("seantalts/stanli", subdir = "r")

# or from a checkout
# R CMD INSTALL r
```

On macOS and Windows the R-universe route installs a prebuilt binary. A
source install builds the 40 KB C bridge, so it wants the toolchain R
already expects for source packages: Xcode command line tools on macOS,
`r-base-dev` on Debian and Ubuntu, Rtools on Windows. The sampler
itself is not compiled here -- it arrives prebuilt below.

Then, once per machine:

```r
stanli_install()
```

That downloads the ~9-12 MB runtime for your platform into
`tools::R_user_dir("stanli", "cache")`, under the release the package
was built against, so the binding and the library always agree: after a
package upgrade the old runtime is simply not found and this step runs
once more. Nothing is fetched without it. Set `STANLI_RUNTIME` to use a
library you built yourself.

## Use

```r
library(stanli)

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)

# Fit a single-path Pathfinder approximation first, then draw one
# reproducible NUTS start per chain from it.
fit_pf <- sample_model(
  m, chains = 4, seed = 303,
  pathfinder_init = list(num_iterations = 500L, num_elbo_draws = 25L)
)

summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
stanli_diagnose(fit)  # divergences, treedepth, E-BFMI, R-hat, ESS
as_draws_array(fit)   # a posterior::draws_array
```

Sampling prints periodic per-chain progress, elapsed warmup and sampling
times, and any divergent-transition or maximum-treedepth warnings. Set
`refresh = 0` for a quiet run. Exact per-chain times and problem counts are
also available in `fit$report`. When the package is deliberately paired with
a compatible older runtime, sampling still works but these new report fields
are `NA` and `fit$report$available` is `FALSE`.

Four chains of eight schools (8000 draws, full summary and diagnostics)
take about 70 ms, because the model is lowered to a graph over
precompiled kernels rather than translated to C++ and compiled.

`summary()` uses stan's own estimators (rank-normalized split R-hat,
bulk/tail ESS, MCSE), so the numbers agree with `stansummary`.
`stanli_diagnose()` runs the checks a Bayesian workflow turns on,
including E-BFMI, the one that catches a badly explored heavy tail.
`optimize_model()` finds the posterior mode by L-BFGS, and its
`$unconstrained` element is what `sample_model(init = )` takes.
`unconstrain(m, list(mu = 0.5, sigma = 1.2))` builds the same vector from
starting values on the constrained scale, naming the parameter when one is
missing, the wrong size, or outside its support.
`pathfinder_init = list()` uses single-path Pathfinder defaults; its supported
overrides are `num_iterations`, `num_elbo_draws`, `history_size`, and
`init_radius`. It is mutually exclusive with explicit `init`, and does not
perform PSIS resampling.

Chains run in parallel by default. Threading does not change the
answer: each chain owns its executor and its RNG stream, so a parallel
run is byte-identical to a sequential one, which the test suite
asserts.

## Bayesian workflow packages

Install `bayesplot`, `loo`, and `tidybayes` as needed; they are optional.
`bayesplot::nuts_params(fit)`, `bayesplot::log_posterior(fit)`,
`bayesplot::rhat(fit)`, and `bayesplot::neff_ratio(fit)` work directly, as does
`tidybayes::spread_draws(fit, mu, theta[j])`. With pointwise `log_lik` declared
in generated quantities, use `loo::loo(fit)` and `loo::loo_compare(fit, fit2)`.
The ESS ratio uses bulk ESS; bayesplot's CmdStanMCMC method uses basic ESS.

`summary(fit)` deliberately keeps the **stansummary** table. For cmdstanr's
default `fit$summary()` table, use this one-line alternative:

```r
posterior::summarise_draws(as_draws_array(fit))
```

Its columns are `variable`, `mean`, `median`, `sd`, `mad`, `q5`, `q95`, `rhat`,
`ess_bulk`, and `ess_tail`. Posterior's other conversions, subsetting, chain
merging, and thinning work on the same draws array. Saved warmup is excluded
from `as_draws_array()` and ecosystem methods; use `inc_warmup = TRUE` to include
it in the array. `summary(fit)` continues to summarize the raw stored draws.

See [Coming from cmdstanr](../docs/from-cmdstanr.md) for the API translation,
plotting, LOO, and saving fits, and the [course quickstart](../docs/teaching.md)
for binary installation, offline labs, and a self-contained example. These
are plain Markdown guides so package checks need no runtime to build vignettes.

### Native RStan fits

`as_stanfit(fit)` builds a `stanli_stanfit`, an S4 subclass of `rstan::stanfit`,
directly from stored arrays. It retains the stanli model for density and
transform operations, without CSV files or C++ model compilation:

```r
sf <- as_stanfit(fit)
rstan::extract(sf)
rstan::traceplot(sf, pars = "mu")
q <- fit$unconstrained[1, 1, ]
pars <- rstan::constrain_pars(sf, q)
rstan::unconstrain_pars(sf, pars)
rstan::log_prob(sf, q, gradient = TRUE)
rstan::grad_log_prob(sf, q)
rstan::constrain_pars(sf, q)
rstan::get_num_upars(sf)
```

Use the parameter names declared by your model for the `unconstrain_pars()`
list. `constrain_pars()` returns parameters, transformed parameters, and generated
quantities with their original dimensions; random generated quantities advance
the retained model's RNG stream. The methods use RStan's defaults, including
the parameter-transform Jacobian. `adjust_transform = FALSE` errors explicitly:
the current runtime cannot remove that Jacobian at evaluation time. Density
methods also reject `STANLI_LITE_LP` runtimes, whose constants differ.

RStan is optional and loads on conversion, not during ordinary stanli startup.
[CRAN provides rstan 2.32.7 binaries](https://CRAN.R-project.org/package=rstan)
for supported macOS and Windows R versions (verified September 2026); install
with `install.packages("rstan", type = "binary")` to avoid requiring a C++
toolchain. Linux classrooms should provision a compatible binary installation.

Extraction, summaries, sampler diagnostics, plotting, ordinary LOO, and
ShinyStan work. With a live model and pointwise `log_lik`, RStan's LOO moment
matching also works: `loo::loo_moment_match(sf, loo = loo::loo(sf), cores = 1)`.
The result passes `inherits(sf, "stanfit")` and `methods::is(sf, "stanfit")`;
consumers that require exact class equality may need adaptation. Operations
that bypass method dispatch and require RStan's own C++ instance, including
native RStan resampling, remain unsupported. Continue sampling with
`sample_model()`.

`saveRDS()` preserves draws and parameter-count metadata, but the live model
pointer does not survive loading or transfer to a serialized worker. Recreate
the model using the **same source and data** and attach it explicitly:

```r
saved <- readRDS("fit.rds")
m <- stanli_model(file = "model.stan", data = original_data)
sf <- as_stanfit(saved, model = m) # accepts saved stanli_fit or stanli_stanfit
```

No compilation or download happens automatically on restore. Without a live
model, extraction and summaries still work; density/transform calls give an
attachment hint. Use `as_stanfit(fit, model = NULL)` to omit the model entirely.

Per-chain timings are retained when available (`NA` otherwise). Initial values
and adaptation text are empty because the actual initial state is not retained.
Shapes come from column names, so declarations with zero elements cannot be
recovered. Conversion from a `stanli_fit` requires the sampling metadata saved
by the current package. Set R's seed before conversion when reproducible
permuted extraction is needed.

See [stanfit compatibility notes](../docs/stanfit-compatibility.md) for checked
consumer versions, independently recorded RStan references, and CLI validation.

## How it is put together

Two pieces are not in the package, for two different reasons.

**The runtime** is a ~29 MB shared library holding stan-math, every density
kernel, and the interpreter. It is published separately so installing the R
bridge never compiles all of that; `stanli_install()` downloads the prebuilt
library instead. Because the package and the
library are separately versioned, they can drift, and drift here would
not crash: `stanli_sample_opts` is a struct this package declares a
copy of, so mismatched layouts would read fields at the wrong offsets
and sample happily from the wrong seed. So the C ABI carries a layout
version (`stanli_abi_version()`) and loading refuses on a mismatch with
a message saying which side to update.

**The Stan compiler** uses the embedded stanc3 and stanli OCaml pipeline in the
macOS and Linux release runtimes. On native hosts without an embedded compiler,
`STANLI_STANC` remains the first choice as an explicit stock-compiler override
for bisects. Otherwise R prefers
`stanli-compile` beside the runtime, then stock `stanc` beside the runtime or
on `PATH`, and finally stanc3 compiled to JavaScript through V8. The portable
producer is never taken from `PATH`, because it must match the runtime's
schema. Launch errors, nonzero exits, and empty output are reported; invalid
portable output is rejected when decoded. None causes an automatic retry
through stock stanc.

The V8 and webR helpers are ready for a later JavaScript producer switch: they
use `stanli_compile()` when that export exists and call `stanc()` only when it
does not. An error from a selected portable producer is final. The JavaScript
file currently bundled with the package still exports only `stanc()`, so this
readiness path does not change what the R package ships. The switch follows a
runtime release containing the compact-v2 reader.

The v0.9.2 compatibility runtime carries pristine `stanc.exe`; the selection
logic falls through to it automatically. Windows runtime tarballs built from
this revision add `stanli-compile.exe` beside that rollback compiler for one
release cycle. Both are short-lived subprocesses; there is no OCaml compiler
DLL. The bundled JavaScript compiler records its exact stanc3 repository,
revision, and content hash. CI verifies that provenance and compiles valid and
invalid models through the file on Linux, macOS, and Windows.

## What is not here yet

`optimize_model()` returns the posterior **mode**. CmdStan's `optimize`
defaults to `jacobian=0`, the penalized maximum likelihood, which
stanli cannot offer: the change-of-variables Jacobian is folded into
the graph when the model is lowered.

## Teaching collections and migration

See [Teaching with Stanli](../docs/teaching-support.md) for tested Rethinking,
brms, and educational models, numerical and performance evidence, and working
examples. The [cmdstanr translation table](../docs/from-cmdstanr.md) covers
common operations and differences.

## Native R-style fit methods without RStan

The development package includes `as_rfit()`, a view of an existing fit with
RStan-style method signatures and result layouts:

```r
native <- as_rfit(fit)
post <- stanli::extract(native)             # named, draw-first parameter arrays
summary(native)$summary                    # RStan-shaped summary matrix
stanli::get_sampler_params(native)          # one matrix per chain
stanli::get_elapsed_time(native)            # chain-by-phase seconds
```

This interface ships inside Stanli. It requires no adapter package and never
loads RStan. Summaries use the optional posterior package and need no native
runtime; extraction and timings work directly from saved arrays. Permuted
extraction excludes warmup and preserves joint draws using stored per-chain
permutations. Set R's seed before `as_rfit()` to reproduce those permutations.

The fit retains class `stanli_fit` for the existing bayesplot, loo, tidybayes,
and draw-conversion methods. Its RStan-shaped `summary()` uses posterior's
basic ESS and split R-hat. The ESS implementation differs from RStan's legacy
estimator, so `n_eff` and `se_mean` need not match RStan. Existing, unconverted
Stanli fits retain their original summary behavior.

A `stanli_rfit` owns its S3 class; it is not an RStan S4 object. Consumers that
call `rstan::extract()` must dispatch to `stanli::extract()` for it. Consumers
requiring RStan's slots can still use the separate optional `as_stanfit()`
conversion, which requires RStan. Recreate the live model after serialization
before using model-dependent operations.

## CmdStanR-style fit methods

The development R package also offers `as_cstanfit(fit)`, a native object with
`$draws()`, `$summary()`, `$sampler_diagnostics()`, `$metadata()`, `$num_chains()`,
`$time()`, and `$loo()` methods. It needs neither cmdstanr nor rstan. This is
useful for integrations that already consume those methods, such as ulam.
It keeps its own `stanli_cstanfit` class; consumers with CmdStanR class checks
must explicitly accept it. Use `$stanli_fit()` to retrieve the original fit.

```r
fit <- sample_cstan(
  model_code = "parameters { real mu; } model { mu ~ normal(0, 1); }",
  chains = 4, parallel_chains = 4, iter_warmup = 500, iter_sampling = 1000,
  init = list(mu = 0), seed = 42, refresh = 0
)
fit$draws("mu")
fit$summary("mu", "mean", "sd", "rhat", "ess_bulk")
```

`sample_cstan()` translates CmdStanR-style sampler arguments into native
sampling and then returns `as_cstanfit()`. Initial values must be complete
constrained lists, a list per chain, or a function returning a list. Unknown
sampler options fail explicitly. C++ compilation options and within-chain
threading are unsupported.

Summaries use posterior's rank-normalized R-hat and bulk/tail ESS, matching
CmdStanR's default summary calculations. The existing `as_rfit()` interface
continues to provide RStan-style tables and basic ESS/R-hat. Iteration counts
in `$metadata()` are before thinning; draw arrays contain retained iterations.
Diagnostics have a stable alphabetical column order. Overall wall time is
reported as unavailable because the native report only retains per-chain times.
Saved-draw methods need the Stanli R package and optional posterior/loo packages,
but no native runtime. CSV/executable methods and LOO moment matching are not
provided by this adapter.
