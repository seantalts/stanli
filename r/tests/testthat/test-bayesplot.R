test_that("bayesplot tables match CmdStanMCMC and posterior draw indices", {
  skip_without_runtime()
  skip_if_not_installed("bayesplot")
  skip_if_not_installed("posterior")
  fit <- sample_model(es_model(), chains = 4, seed = 1, refresh = 0)
  np <- bayesplot::nuts_params(fit)
  lp <- bayesplot::log_posterior(fit)
  pars <- c("accept_stat__", "stepsize__", "treedepth__", "n_leapfrog__",
            "divergent__", "energy__")
  expect_identical(names(np), c("Chain", "Iteration", "Parameter", "Value"))
  expect_identical(names(lp), c("Chain", "Iteration", "Value"))
  expect_identical(levels(np$Parameter), pars)
  expect_equal(nrow(np), 1000L * 4L * length(pars))
  expect_equal(nrow(lp), 1000L * 4L)

  # Exercise bayesplot's actual upstream methods with the same numeric arrays.
  # This tests the contract without needing CmdStan installed or sampled.
  proxy <- structure(list(
    sampler_diagnostics = function() {
      posterior::as_draws_array(fit$sampler[, , pars, drop = FALSE])
    },
    draws = function(variable, inc_warmup = FALSE) {
      posterior::as_draws_array(fit$sampler[, , variable, drop = FALSE])
    }
  ), class = "CmdStanMCMC")
  expect_identical(np, bayesplot::nuts_params(proxy))
  expect_identical(lp, bayesplot::log_posterior(proxy))
  selected <- c("energy__", "divergent__")
  expect_identical(bayesplot::nuts_params(fit, pars = selected),
                   bayesplot::nuts_params(proxy, pars = selected))
  expect_error(bayesplot::nuts_params(fit, pars = "bad"))

  df <- posterior::as_draws_df(as_draws_array(fit))
  expect_identical(lp$Chain, df$.chain)
  expect_identical(lp$Iteration, df$.iteration)
  expect_equal(lp$Value, as.vector(fit$sampler[, , "lp__"]))
  for (par in pars) {
    rows <- np[np$Parameter == par, ]
    expect_identical(rows$Chain, df$.chain)
    expect_identical(rows$Iteration, df$.iteration)
    expect_equal(rows$Value, as.vector(fit$sampler[, , par]))
  }
  s <- summary(fit)
  expect_equal(bayesplot::rhat(fit), setNames(s$rhat, s$variable))
  expect_equal(bayesplot::neff_ratio(fit),
               setNames(s$ess_bulk / 4000, s$variable))
  expect_equal(names(bayesplot::rhat(fit, pars = "theta")),
               paste0("theta[", 1:8, "]"))
  expect_equal(bayesplot::neff_ratio(fit, pars = "mu"),
               setNames(s$ess_bulk[s$variable == "mu"] / 4000, "mu"))
  expect_error(bayesplot::rhat(fit, pars = "unknown"), "unknown variable")

  # All tutorial acceptance calls must construct plots without warnings.
  expect_warning(plots <- list(
    bayesplot::mcmc_nuts_divergence(np, lp),
    bayesplot::mcmc_nuts_energy(np),
    bayesplot::mcmc_nuts_acceptance(np, lp),
    bayesplot::mcmc_nuts_treedepth(np, lp),
    bayesplot::mcmc_parcoord(as_draws_array(fit), np = np),
    bayesplot::mcmc_rhat(bayesplot::rhat(fit)),
    bayesplot::mcmc_neff(bayesplot::neff_ratio(fit)),
    bayesplot::mcmc_trace(as_draws_array(fit), pars = "mu", np = np)
  ), NA)
  # These three upstream functions return composite grids, including for
  # CmdStanMCMC. The remaining acceptance calls return ggplot objects.
  for (i in c(1L, 3L, 4L)) expect_s3_class(plots[[i]], "bayesplot_grid")
  for (i in c(2L, 5L, 6L, 7L, 8L)) expect_s3_class(plots[[i]], "ggplot")
})

test_that("saved, thinned warmup is excluded consistently", {
  skip_without_runtime()
  skip_if_not_installed("bayesplot")
  fit <- sample_model(es_model(), chains = 2, seed = 2, warmup = 101,
                      samples = 201, thin = 3, save_warmup = TRUE, refresh = 0)
  expect_equal(fit$warmup_draws, 34L)
  expect_equal(unname(dim(as_draws_array(fit))[1L]), 67L)
  expect_equal(unname(dim(as_draws_array(fit, inc_warmup = TRUE))[1L]), 101L)
  expect_equal(nrow(bayesplot::log_posterior(fit)), 67L * 2L)
  expect_equal(nrow(bayesplot::log_posterior(fit, inc_warmup = TRUE)), 101L * 2L)
  post <- fit
  post$draws <- fit$draws[-seq_len(34L), , , drop = FALSE]
  s <- summary(post)
  expect_equal(bayesplot::neff_ratio(fit),
               setNames(s$ess_bulk / (67 * 2), s$variable))
  expect_equal(bayesplot::rhat(fit), setNames(s$rhat, s$variable))
})
