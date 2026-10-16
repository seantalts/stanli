# Coming from cmdstanr

stanli uses functions and ordinary R lists. Supply data when constructing the
model, then pass that model to sampling or optimization. These examples assume
`library(stanli)` and the optional workflow packages are installed.

| cmdstanr | stanli |
|---|---|
| `m <- cmdstan_model("model.stan")` | `m <- stanli_model("model.stan", data = data)` |
| `m$compile()` | No separate call. `stanli_model()` compiles, in milliseconds for small models. |
| `m$sample(data = data, chains = 4, parallel_chains = 4, iter_warmup = 1000, iter_sampling = 1000, seed = 1, adapt_delta = 0.8, max_treedepth = 10, init = init, refresh = 100)` | `sample_model(m, chains = 4, parallel_chains = 4, warmup = 1000, samples = 1000, seed = 1, delta = 0.8, max_depth = 10, init = q, refresh = 100)`; see initialization below. |
| `m$sample(data = data, metric = "dense_e", inv_metric = M, step_size = 0.5, adapt_engaged = FALSE)` | `sample_model(m, metric = "dense_e", inv_metric = M, step_size = 0.5, adapt_engaged = FALSE)`, with the same argument names. `fit$inv_metric` and `fit$step_size` hold what each chain adapted, so they can start a later fit; `fit$inv_metric()` and `fit$metadata()$step_size_adaptation` are the cmdstanr equivalents. |
| `fit$summary()` | `posterior::summarise_draws(as_draws_array(fit))` for the cmdstanr default table; `summary(fit)` for the stansummary table. |
| `fit$draws()` | `as_draws_array(fit)` |
| `fit$draws("mu")` | `posterior::subset_draws(as_draws_array(fit), variable = "mu")` |
| `fit$diagnostic_summary()` or `fit$cmdstan_diagnose()` | `stanli_diagnose(fit)` prints a report; `fit$report` holds per-chain timings, divergence counts, and treedepth counts. |
| `fit$sampler_diagnostics()` | `fit$sampler` is the raw array, including `lp__`; `bayesplot::nuts_params(fit)` gives the plotting table, excluding `lp__` by default. |
| `m$optimize(data = data, jacobian = TRUE)` | `optimize_model(m)` includes the transform Jacobian. CmdStan's default `jacobian = FALSE` gives penalized maximum likelihood and has no stanli equivalent. |
| `m$pathfinder(data = data, num_paths = 1, draws = 1000)` | `pathfinder_model(m, draws = 1000)`: single-path Pathfinder. cmdstanr's default of four paths with PSIS resampling has no equivalent; see below. `sample_model(m, pathfinder_init = list())` uses the same single path to initialize NUTS. |
| `m$variational(data = data, algorithm = "meanfield", iter = 10000, draws = 1000)` | `variational_model(m, algorithm = "meanfield", iter = 10000, draws = 1000)`; `"fullrank"` likewise. |
| `m$laplace(data = data, mode = opt, draws = 1000)` | `laplace_model(m, mode = optimize_model(m), draws = 1000)`, or leave `mode` out to find it first. Always with the Jacobian, which is also cmdstanr's default. |
| `m$sample(data = data, fixed_param = TRUE)` | `sample_model(m, fixed_param = TRUE)` |
| `fit$draws()` of those fits, with `lp__` and `lp_approx__` | `as_draws_array(fit, include_sampler = TRUE)`; the two columns are also in `fit$sampler`. |
| `fit$loo()` | `loo::loo(fit)` with pointwise `log_lik` in generated quantities. |
| `fit$save_object("fit.rds")` | Strip the live model, then `saveRDS(saved, "fit.rds")`; see persistence below. |
| `bayesplot::mcmc_trace(fit$draws(), np = bayesplot::nuts_params(fit))` | `bayesplot::mcmc_trace(as_draws_array(fit), np = bayesplot::nuts_params(fit))` |
| `rstan::read_stan_csv(fit$output_files())` | `as_stanfit(fit)`, built in memory with no CSV files. |

Code written against CmdStanR's fit methods can use `sample_cstan()`,
`cstan_model()` or `as_cstanfit()` instead of translating. Code written against
RStan can use `as_stanfit()` or `as_rfit()`. The
[fit adapters table](../r/README.md#fit-adapters) says what each returns,
needs, and cannot do.

## Initialization and saved warmup

cmdstanr initial values are constrained parameter lists, functions or files.
stanli's `init` is an unconstrained numeric vector, or a matrix with one row
per chain. Convert a complete constrained start with one line:

```r
q <- unconstrain(m, list(mu = 0, tau = 1, theta_tilde = rep(0, 8)))
```

The names must match your model. For separate chain starts, apply `unconstrain()`
to each list and combine the vectors with `rbind()`. Omit `init` for random
starts; `init_radius = 0` starts at the unconstrained origin. `sample_cstan()`
takes cmdstanr-style constrained lists directly.

With `save_warmup = TRUE`, raw `fit$draws` and `fit$sampler` include saved
warmup, and `summary(fit)` summarizes that raw array. `as_draws_array()`, the
ecosystem diagnostics, LOO, and tidybayes leave warmup out unless you pass
`inc_warmup = TRUE`. Iteration numbers index retained draws within each chain,
starting at one after warmup. Fits saved by older versions lack warmup
metadata; set `fit$warmup_draws` to the number of saved warmup rows per chain.

## Tutorial plotting and tidy draws

These bayesplot calls work directly:

```r
np <- bayesplot::nuts_params(fit)
lp <- bayesplot::log_posterior(fit)
bayesplot::mcmc_nuts_divergence(np, lp)
bayesplot::mcmc_nuts_energy(np)
bayesplot::mcmc_nuts_acceptance(np, lp)
bayesplot::mcmc_nuts_treedepth(np, lp)
bayesplot::mcmc_parcoord(as_draws_array(fit), np = np)
bayesplot::mcmc_rhat(bayesplot::rhat(fit))
bayesplot::mcmc_neff(bayesplot::neff_ratio(fit))
bayesplot::mcmc_trace(as_draws_array(fit), pars = "mu", np = np)
tidybayes::spread_draws(fit, mu, theta[j])
```

`nuts_params()` and `log_posterior()` return the same columns as
[bayesplot's CmdStanMCMC methods](https://github.com/stan-dev/bayesplot/blob/master/R/bayesplot-extractors.R).
stanli's `neff_ratio()` divides bulk ESS by the retained post-warmup draws;
bayesplot's CmdStanMCMC method uses basic ESS. For that estimator:

```r
posterior::summarise_draws(as_draws_array(fit), ratio = function(x) posterior::ess_basic(x) / length(x))
```

For a posterior predictive check, declare `vector[N] y_rep` in generated
quantities and fill it with draws from the observation distribution. Replace
`as_draws_matrix(fit$draws("y_rep"))` in a tutorial with:

```r
yrep <- posterior::as_draws_matrix(posterior::subset_draws(as_draws_array(fit), variable = "y_rep"))
bayesplot::ppc_dens_overlay(y, yrep[1:50, , drop = FALSE])
```

## Pointwise log likelihood and LOO

For a normal observation model, add this block to the Stan source:

```stan
generated quantities {
  vector[N] log_lik;
  for (n in 1:N) log_lik[n] = normal_lpdf(y[n] | mu, sigma);
}
```

After sampling:

```r
ll <- log_lik(fit)  # iteration x chain x observation, in Stan column order
loo1 <- loo::loo(fit)
loo::loo_compare(fit, fit2)  # fits must describe the same observations
loo::loo_compare(list(model1 = loo1, model2 = loo::loo(fit2)))
```

Relative efficiency is computed with the chain axis intact. A differently
named variable works with `loo::loo(fit, variable = "pointwise")`. Pointwise
likelihoods are never made up from `lp__`.

## Persistence

R cannot serialize a live external pointer. Save the fit with the model
removed; the original stays usable in the current session:

```r
saved <- fit
saved$model <- NULL
saveRDS(saved, "fit.rds")
fit <- readRDS("fit.rds")
```

The class, draws, sampler statistics, unconstrained draws, seed, and report
round-trip, and posterior, bayesplot, tidybayes, and LOO work after loading.
`summary()`, `stanli_diagnose()`, `rhat()`, and `neff_ratio()` need the stanli
runtime installed, but no model. Keep your Stan source and data. To evaluate
the model or sample again, rebuild it with `stanli_model(file, data = data)`
and reattach it as `fit$model`. For an `as_stanfit()` result, see
[the adapter notes](../r/README.md#as_stanfit).

## Features without a stanli equivalent

- Standalone `generate_quantities()`.
- cmdstanr's `$output()` and per-chain CSV files: R draws live in memory.
  The same goes for Pathfinder's single-path output files and ADVI's
  diagnostic file; ADVI's lower-bound trace is printed with `refresh = 1`.
- Optimization and `laplace()` with `jacobian = FALSE`.
- A `mode` for `laplace()` given as constrained values in a file: pass
  `unconstrain()`'s result.
- Multi-path Pathfinder (`num_paths` above 1, with PSIS resampling); see
  [Limits](../r/README.md#limits).

The [teaching page](teaching.md) covers classroom installation and an
eight-schools example that needs no model files.
