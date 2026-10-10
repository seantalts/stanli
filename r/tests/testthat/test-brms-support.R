# What a brms backend needs from this package, and brms itself where it is
# installed.

brms_support_fit <- function() {
  code <- "
    data { int<lower=0> N; vector[N] y; }
    parameters { real mu; real<lower=0> sigma; vector[2] z; }
    model { y ~ normal(mu, sigma); z ~ std_normal(); sigma ~ exponential(1); }
    generated quantities { vector[N] log_lik;
      for (n in 1:N) log_lik[n] = normal_lpdf(y[n] | mu, sigma); }"
  model <- stanli_model(code = code, data = list(N = 5L, y = c(-1, 0, 0.5, 1, 2)))
  sample_model(model, chains = 2, warmup = 100, samples = 100, refresh = 0,
               delta = 0.9, max_depth = 8)
}

test_that("as_stanfit drops excluded variables from the stored draws only", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- brms_support_fit()
  all_pars <- as_stanfit(fit)
  some <- as_stanfit(fit, exclude = c("z", "log_lik", "not_a_variable"))
  expect_identical(some@sim$pars_oi, c("mu", "sigma", "lp__"))
  expect_identical(some@sim$fnames_oi, c("mu", "sigma", "lp__"))
  expect_identical(names(some@sim$samples[[1L]]), c("mu", "sigma", "lp__"))
  expect_identical(some@sim$samples[[1L]]$mu, all_pars@sim$samples[[1L]]$mu)
  # the live model still has every parameter
  expect_identical(rstan::get_num_upars(some), 4L)
  expect_equal(rstan::log_prob(some, c(0, 0, 0, 0)),
               rstan::log_prob(all_pars, c(0, 0, 0, 0)))
  expect_error(as_stanfit(fit, exclude = 1), "exclude must be a character vector")
})

test_that("a model can be reattached after variables were dropped or renamed", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- brms_support_fit()
  sf <- as_stanfit(fit, exclude = "z")
  # what brms does to the names it shows
  sf@sim$fnames_oi[1L] <- "b_mu"
  saved <- tempfile(fileext = ".rds")
  saveRDS(sf, saved)
  restored <- readRDS(saved)
  expect_error(rstan::log_prob(restored, c(0, 0, 0, 0)), "no live stanli model")
  again <- as_stanfit(restored, model = fit$model)
  expect_equal(rstan::log_prob(again, c(0, 0, 0, 0)),
               rstan::log_prob(sf, c(0, 0, 0, 0)))
  other <- stanli_model(code = "parameters { real mu; } model { mu ~ std_normal(); }")
  expect_error(as_stanfit(restored, model = other), "model must match")
})

test_that("starting values may leave parameters out", {
  skip_without_runtime()
  model <- brms_support_fit()$model
  # mu, sigma (positive), z[1:2]
  full <- unconstrain(model, list(mu = 0.5, sigma = 2, z = c(1, -1)))
  expect_equal(full, c(0.5, log(2), 1, -1))
  expect_error(unconstrain(model, list(sigma = 2)), "no starting value")

  some <- unconstrain(model, list(sigma = 2), partial = TRUE, seed = 11)
  expect_length(some, 4L)
  expect_equal(some[2L], log(2))
  expect_true(all(abs(some[-2L]) <= 2))
  expect_false(any(some[-2L] == 0))
  # a seed makes the draw repeatable and leaves R's own stream alone
  set.seed(3); before <- stats::runif(1)
  set.seed(3)
  expect_identical(unconstrain(model, list(sigma = 2), partial = TRUE, seed = 11), some)
  expect_identical(stats::runif(1), before)
  expect_false(identical(
    unconstrain(model, list(sigma = 2), partial = TRUE, seed = 12), some))
  # a radius of zero starts the rest at the origin
  expect_equal(unconstrain(model, list(z = c(3, 4)), partial = TRUE, init_radius = 0),
               c(0, 0, 3, 4))
  # nothing given is a fully random start; a complete list is used as it is
  expect_length(unconstrain(model, list(), partial = TRUE, seed = 1), 4L)
  expect_identical(
    unconstrain(model, list(mu = 0.5, sigma = 2, z = c(1, -1)), partial = TRUE), full)
  # out-of-support values are still refused
  expect_error(unconstrain(model, list(sigma = -1), partial = TRUE))

  fit <- sample_model(model, chains = 2, warmup = 50, samples = 50, refresh = 0,
                      init = rbind(some, some))
  expect_identical(dim(fit$draws)[1:2], c(50L, 2L))
})

test_that("each chain records its sampler settings as rstan does", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  sf <- as_stanfit(brms_support_fit())
  control <- attr(sf@sim$samples[[2L]], "args")$control
  expect_identical(control, list(adapt_delta = 0.9, max_treedepth = 8))
})

test_that("stanli_check_syntax accepts valid code and reports invalid code", {
  skip_without_runtime()
  expect_true(stanli_check_syntax("parameters { real mu; } model { mu ~ normal(0, 1); }"))
  expect_error(stanli_check_syntax("parameters { real mu } model { }"), "rror")
  expect_error(stanli_check_syntax(c("a", "b")), "single string")
})

test_that("log_lik defers to rstantools for objects of other packages", {
  skip_if_not_installed("rstantools")
  other <- structure(list(), class = "not_a_stanli_object")
  registerS3method("log_lik", "not_a_stanli_object", function(object, ...) "theirs",
                   envir = asNamespace("rstantools"))
  expect_identical(log_lik(other), "theirs")
})

test_that("a stanli fit completes a brmsfit that brms post-processes", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("brms")
  set.seed(1)
  d <- data.frame(g = factor(rep(1:5, each = 8)), x = rnorm(40))
  d$y <- 1 + 0.5 * d$x + rnorm(5)[d$g] + rnorm(40, sd = 0.7)
  formula <- y ~ x + (1 | g)
  model <- stanli_model(code = brms::make_stancode(formula, d),
                        data = unclass(brms::make_standata(formula, d)))
  fit <- sample_model(model, chains = 2, warmup = 300, samples = 300, refresh = 0)
  # brms's own backend for tests takes a finished stanfit
  bf <- brms::brm(formula, d, backend = "mock", mock_fit = as_stanfit(fit))
  expect_s3_class(bf, "brmsfit")
  expect_true(all(c("b_Intercept", "b_x", "sd_g__Intercept", "sigma") %in%
                    posterior::variables(bf)))
  expect_identical(dim(brms::posterior_predict(bf)), c(600L, 40L))
  expect_identical(dim(brms::log_lik(bf)), c(600L, 40L))
  expect_s3_class(suppressWarnings(brms::loo(bf)), "loo")
  expect_s3_class(brms::conditional_effects(bf), "brms_conditional_effects")
})

test_that("brm(backend = \"stanli\") works where brms provides it", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("brms")
  skip_if_not("stanli" %in% brms:::backend_choices(),
              "this brms has no stanli backend")
  set.seed(1)
  d <- data.frame(y = rnorm(30), x = rnorm(30))
  bf <- brms::brm(y ~ x, d, backend = "stanli", chains = 2, iter = 400,
                  seed = 1, silent = 2, refresh = 0)
  expect_identical(bf$backend, "stanli")
  expect_identical(posterior::ndraws(bf), 400L)
})
