test_that("posterior's workflow preserves variables, axes, and values", {
  skip_without_runtime()
  skip_if_not_installed("posterior")
  fit <- sample_model(es_model(), chains = 4, seed = 1, refresh = 0)
  draws <- as_draws_array(fit)
  summary <- posterior::summarise_draws(draws)
  expect_identical(summary$variable, fit$columns)
  expect_identical(names(summary), c("variable", "mean", "median", "sd", "mad",
                                    "q5", "q95", "rhat", "ess_bulk", "ess_tail"))
  df <- posterior::as_draws_df(draws)
  expect_equal(nrow(df), 4000)
  expect_equal(df$mu, as.vector(fit$draws[, , "mu"]))
  rv <- posterior::as_draws_rvars(draws)
  expect_equal(dim(rv$theta), 8L)
  expect_equal(posterior::variables(posterior::subset_draws(draws, variable = "mu")),
               "mu")
  merged <- posterior::merge_chains(draws)
  expect_equal(unname(posterior::nchains(merged)), 1L)
  expect_equal(unname(posterior::ndraws(merged)), 4000L)
  thinned <- posterior::thin_draws(draws, thin = 2)
  expect_equal(unname(posterior::ndraws(thinned)), 2000L)
  expect_equal(as.vector(thinned[, , "mu"]),
               as.vector(fit$draws[seq(1, 1000, by = 2), , "mu"]))
})

test_that("tidybayes spreads scalar and indexed variables directly", {
  skip_without_runtime()
  skip_if_not_installed("tidybayes")
  fit <- sample_model(es_model(), chains = 2, warmup = 100, samples = 100,
                      seed = 4, refresh = 0)
  spread <- tidybayes::spread_draws(fit, mu, theta[j])
  expect_equal(nrow(spread), 200L * 8L)
  expect_setequal(spread$j, 1:8)
  expect_setequal(names(spread), c(".chain", ".iteration", ".draw", "mu", "j", "theta"))
  expect_equal(spread$mu, fit$draws[cbind(spread$.iteration, spread$.chain,
                                       match("mu", fit$columns))])
  expect_equal(spread$theta, fit$draws[cbind(spread$.iteration, spread$.chain,
                                          match(paste0("theta[", spread$j, "]"),
                                                fit$columns))])
})

test_that("saved fits retain the workflow in a fresh R session", {
  skip_without_runtime()
  skip_if_not_installed("posterior")
  skip_if_not_installed("bayesplot")
  skip_if_not_installed("loo")
  skip_if_not_installed("tidybayes")
  skip_if_not_installed("callr")
  fit <- sample_model(log_lik_model(), chains = 4, warmup = 500, samples = 1000,
                      refresh = 0)
  saved <- fit
  saved$model <- NULL
  path <- tempfile(fileext = ".rds")
  on.exit(unlink(path))
  saveRDS(saved, path)
  restored <- callr::r(function(path) {
    library(stanli)
    x <- readRDS(path)
    list(draws = as_draws_array(x), summary = summary(x),
         report = stanli_diagnose(x), rhat = bayesplot::rhat(x),
         loo = loo::loo(x)$estimates, tidy = tidybayes::tidy_draws(x),
         model = x$model)
  }, list(path), libpath = .libPaths())
  expect_identical(restored$draws, as_draws_array(fit))
  expect_equal(restored$summary, summary(fit))
  expect_equal(restored$rhat, bayesplot::rhat(fit))
  expect_equal(restored$loo, loo::loo(fit)$estimates)
  expect_equal(nrow(restored$tidy), 4000L)
  expect_null(restored$model)
})


test_that("posterior predictive draws feed the bayesplot tutorial", {
  skip_without_runtime()
  skip_if_not_installed("posterior")
  skip_if_not_installed("bayesplot")
  fit <- sample_model(log_lik_model(), chains = 2, warmup = 100, samples = 100,
                      seed = 8, refresh = 0)
  yrep <- posterior::as_draws_matrix(posterior::subset_draws(
    as_draws_array(fit), variable = "y_rep"))
  expect_equal(unname(dim(yrep)), c(200L, 12L))
  expect_warning(plot <- bayesplot::ppc_dens_overlay(
    seq(-1, 1, length.out = 12), yrep[1:50, , drop = FALSE]), NA)
  expect_s3_class(plot, "ggplot")
})
