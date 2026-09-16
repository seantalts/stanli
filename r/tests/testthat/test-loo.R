test_that("LOO matches the chain-aware matrix path and compares fits", {
  skip_without_runtime()
  skip_if_not_installed("loo")
  skip_if_not_installed("posterior")
  fit <- sample_model(log_lik_model(), chains = 4, warmup = 500, samples = 1000,
                      seed = 1, refresh = 0)
  ll <- log_lik(fit)
  expect_true(is.array(ll))
  expect_false(inherits(ll, "draws"))
  expect_equal(unname(dim(ll)), c(1000L, 4L, 12L))
  expect_identical(dimnames(ll)[[3]], paste0("log_lik[", 1:12, "]"))
  expect_equal(unname(log_lik(fit, variable = "alternate")), unname(ll))
  expect_equal(unname(dim(log_lik(fit, variable = "scalar_ll"))),
               c(1000L, 4L, 1L))
  expect_error(log_lik(fit, variable = "missing"), "generated quantities block")
  expect_error(log_lik(fit, variable = "log"), "No variable 'log'")

  draws <- posterior::as_draws_matrix(as_draws_array(fit))
  mat <- as.matrix(draws[, paste0("log_lik[", 1:12, "]"), drop = FALSE])
  chain_id <- rep(1:4, each = 1000)
  expected <- loo::loo(mat, r_eff = loo::relative_eff(exp(mat), chain_id))
  expect_warning(actual <- loo::loo(fit), NA)
  expect_s3_class(actual, "psis_loo")
  expect_equal(actual$estimates, expected$estimates, tolerance = 1e-10)
  expect_equal(loo::loo(fit, variable = "alternate")$estimates,
               actual$estimates)
  expect_equal(loo::loo(fit, r_eff = rep(1, 12))$estimates,
               loo::loo(ll, r_eff = rep(1, 12))$estimates)
  second <- sample_model(log_lik_model(2), chains = 4, warmup = 500,
                         samples = 1000, seed = 2, refresh = 0)
  comparison <- loo::loo_compare(fit, second)
  expect_s3_class(comparison, "compare.loo")
  expect_equal(unname(as.matrix(comparison)),
               unname(as.matrix(loo::loo_compare(actual, loo::loo(second)))))
})

test_that("log likelihood retains singleton axes and excludes warmup", {
  skip_without_runtime()
  fit <- sample_model(log_lik_model(), chains = 1, warmup = 31, samples = 21,
                      thin = 2, save_warmup = TRUE, refresh = 0)
  expect_equal(unname(dim(log_lik(fit, "scalar_ll"))), c(11L, 1L, 1L))
  expect_equal(as.vector(log_lik(fit, "scalar_ll")),
               as.vector(fit$draws[17:27, , "scalar_ll"]))
  expect_error(log_lik(sample_model(es_model(), chains = 1, warmup = 10,
                                   samples = 10, refresh = 0)),
               "generated quantities block")
})
