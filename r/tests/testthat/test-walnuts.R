walnuts_model <- function() {
  stanli_model(code = "
    parameters { real mu; real<lower=0> tau; }
    model { mu ~ normal(3, 2); tau ~ exponential(1); }
    generated quantities {
      real mu2 = 2 * mu;
      real z = normal_rng(0, 1);
    }")
}

walnuts_fit <- function(model = walnuts_model(), ...) {
  args <- utils::modifyList(list(model, chains = 2, seed = 7, warmup = 300,
                                 samples = 200, refresh = 0), list(...))
  do.call(sample_walnuts, args)
}

test_that("sample_walnuts returns a stanli_fit with WALNUTS's columns", {
  skip_without_runtime()
  model <- walnuts_model()
  fit <- walnuts_fit(model)
  expect_s3_class(fit, "stanli_fit")
  expect_identical(fit$algorithm, "walnuts")
  expect_identical(fit$sampler_backend, "walnuts")
  expect_identical(dim(fit$draws), c(200L, 2L, length(model$columns)))
  expect_identical(dimnames(fit$draws)[[3L]], model$columns)
  expect_identical(model$columns, c("mu", "tau", "mu2", "z"))
  expect_identical(dim(fit$sampler), c(200L, 2L, 2L))
  expect_identical(dimnames(fit$sampler)[[3L]], c("lp__", "stepsize__"))
  expect_identical(dim(fit$unconstrained), c(200L, 2L, 2L))
  expect_identical(fit$chains, 2L)
  expect_identical(fit$samples, 200L)
  expect_identical(fit$warmup, 300L)
  expect_identical(fit$max_error, 0.5)
  expect_null(fit$chain_seeds)
  expect_identical(fit$chain_id, 1L)
  step <- fit$sampler[, , "stepsize__"]
  expect_true(all(step > 0))
  expect_identical(apply(step, 2L, function(s) length(unique(s))), c(1L, 1L))
  expect_true(all(is.finite(fit$draws)))
  expect_length(fit$report$sampling_seconds, 2L)
})

test_that("sample_walnuts draws are constrained and lp__ is the model's density", {
  skip_without_runtime()
  model <- walnuts_model()
  fit <- walnuts_fit(model)
  expect_equal(fit$draws[, , "mu"], fit$unconstrained[, , 1L])
  expect_equal(fit$draws[, , "tau"], exp(fit$unconstrained[, , 2L]))
  expect_identical(fit$draws[, , "mu2"], 2 * fit$draws[, , "mu"])
  z <- fit$draws[, , "z"]
  expect_gt(stats::sd(z), 0.5)
  expect_false(identical(z[, 1L], z[, 2L]))
  for (chain in 1:2) for (i in c(1L, 100L, 200L))
    expect_equal(unname(fit$sampler[i, chain, "lp__"]),
                 log_prob_grad(model, fit$unconstrained[i, chain, ])$lp)
})

test_that("sample_walnuts recovers a known posterior", {
  skip_without_runtime()
  fit <- sample_walnuts(walnuts_model(), chains = 4, seed = 3, warmup = 500,
                        samples = 1000, refresh = 0)
  s <- summary(fit)
  rownames(s) <- s$variable
  # mu ~ normal(3, 2) and tau ~ exponential(1): means 3 and 1, sds 2 and 1.
  truth <- list(mu = c(3, 2), tau = c(1, 1))
  for (name in names(truth)) {
    expect_lt(abs(s[name, "mean"] - truth[[name]][1L]),
              5 * s[name, "mcse_mean"])
    expect_lt(abs(s[name, "sd"] - truth[[name]][2L]), 5 * s[name, "mcse_sd"])
    expect_lt(s[name, "rhat"], 1.01)
  }
})

test_that("sample_walnuts is reproducible per seed and chains differ", {
  skip_without_runtime()
  model <- walnuts_model()
  a <- walnuts_fit(model, chains = 3)
  b <- walnuts_fit(model, chains = 3)
  expect_identical(a$draws, b$draws)
  expect_identical(a$sampler, b$sampler)
  other <- walnuts_fit(model, chains = 3, seed = 8)
  for (i in 1:3) for (j in 1:3) {
    expect_false(identical(a$unconstrained[, i, ], other$unconstrained[, j, ]))
    if (i < j)
      expect_false(identical(a$unconstrained[, i, ], a$unconstrained[, j, ]))
  }
})

test_that("sample_walnuts chain c uses chain_id + c - 1, as sample_model does", {
  skip_without_runtime()
  model <- walnuts_model()
  fit <- walnuts_fit(model, chains = 3)
  for (chain in 2:3) {
    one <- walnuts_fit(model, chains = 1, chain_id = chain)
    expect_identical(one$chain_id, as.integer(chain))
    expect_identical(one$draws[, 1L, ], fit$draws[, chain, ])
    expect_identical(one$sampler[, 1L, ], fit$sampler[, chain, ])
  }
  later <- walnuts_fit(model, chains = 2, chain_id = 2)
  expect_identical(later$draws[, 1L, ], fit$draws[, 2L, ])
})

test_that("sample_walnuts chain 1 starts where NUTS chain 1 starts", {
  skip_without_runtime()
  model <- walnuts_model()
  # A vanishing step leaves NUTS's one draw at its starting point.
  nuts <- sample_model(model, chains = 2, seed = 7, warmup = 0, samples = 1,
                       max_depth = 1, adapt_engaged = FALSE,
                       step_size = 1e-300, refresh = 0)
  start <- nuts$unconstrained[1L, , ]
  expect_true(all(start != 0))
  random <- walnuts_fit(model)
  given <- walnuts_fit(model, init = start)
  expect_identical(given$unconstrained, random$unconstrained)
  expect_identical(given$draws, random$draws)
  other <- walnuts_fit(model, init = start[2:1, ])
  expect_false(identical(other$unconstrained, random$unconstrained))
})

test_that("sample_walnuts takes init and init_radius", {
  skip_without_runtime()
  model <- walnuts_model()
  a <- walnuts_fit(model, init = c(0.5, -0.5))
  b <- walnuts_fit(model, init = matrix(c(0.5, -0.5), 2, 2, byrow = TRUE))
  expect_identical(a$draws, b$draws)
  expect_false(identical(a$draws, walnuts_fit(model)$draws))
  zero <- walnuts_fit(model, init_radius = 0)
  expect_identical(zero$draws, walnuts_fit(model, init = c(0, 0))$draws)
  expect_error(walnuts_fit(model, init = 1:3), "init must be")
  expect_error(walnuts_fit(model, init_radius = -1), "init_radius")
  expect_error(walnuts_fit(model, chain_id = 0), "chain_id")
})

test_that("sample_walnuts fits refuse NUTS-only operations", {
  skip_without_runtime()
  fit <- walnuts_fit()
  expect_error(sample_more(fit), "sample_more\\(\\) needs a NUTS fit.*walnuts")
  expect_error(stanli_diagnose(fit), "needs a NUTS fit.*walnuts")
  expect_error(get_sampler_params(fit), "needs a NUTS fit.*walnuts")
})

test_that("sample_walnuts fits convert to draws and cmdstanr-style objects", {
  skip_without_runtime()
  skip_if_not_installed("posterior")
  fit <- walnuts_fit()
  d <- as_draws_array(fit)
  expect_s3_class(d, "draws_array")
  expect_identical(posterior::variables(d), fit$columns)
  expect_identical(unname(posterior::niterations(d)), 200L)
  expect_identical(unname(posterior::nchains(d)), 2L)
  expect_identical(posterior::variables(as_draws_array(fit, include_sampler = TRUE)),
                   c("lp__", "stepsize__", fit$columns))

  cs <- as_cstanfit(fit)
  expect_identical(posterior::variables(cs$draws()), c("lp__", fit$columns))
  expect_identical(cs$summary("mu")$variable, "mu")
  expect_identical(cs$metadata()$engine, "walnuts")
  expect_identical(cs$metadata()$sampler_diagnostics, character(0))
  expect_identical(dim(cs$sampler_diagnostics())[3L], 0L)
  expect_error(cs$inv_metric(), "sample_walnuts\\(\\).*no Stan inverse metric")
})

test_that("sample_walnuts fits convert to stanfit", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- walnuts_fit()
  sf <- as_stanfit(fit)
  expect_true(inherits(sf, "stanfit"))
  expect_equal(as.vector(as.array(sf)[, , "mu"]), as.vector(fit$draws[, , "mu"]))
})

test_that("sample_walnuts checks its arguments", {
  skip_without_runtime()
  model <- walnuts_model()
  expect_error(sample_walnuts(list()), "stanli_model")
  expect_error(walnuts_fit(model, max_error = 0), "max_error")
  expect_error(walnuts_fit(model, chains = 0), "chains")
  expect_error(walnuts_fit(model, delta = 0.9), "delta is not available")
  expect_error(walnuts_fit(model, thin = 2, save_warmup = TRUE),
               "thin, save_warmup")
})

test_that("sample_walnuts prints progress only when refresh is positive", {
  skip_without_runtime()
  model <- walnuts_model()
  quiet <- capture.output(fit <- walnuts_fit(model, chains = 1))
  expect_identical(quiet, character(0))
  loud <- capture.output(fit <- walnuts_fit(model, chains = 1, refresh = 100))
  expect_true(any(grepl("Chain [1] Iteration:  500 / 500 [100%]  (Sampling)",
                        loud, fixed = TRUE)))
  expect_true(any(grepl("Chain [1] Iteration:    1 / 500 [  0%]  (Warmup)",
                        loud, fixed = TRUE)))
})
