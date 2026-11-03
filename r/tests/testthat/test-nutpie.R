nutpie_eight_schools <- function(...) {
  stanli_model(code = "
    data { int<lower=0> J; array[J] real y; array[J] real<lower=0> sigma; }
    parameters { real mu; real<lower=0> tau; vector[J] eta; }
    transformed parameters { vector[J] theta = mu + tau * eta; }
    model {
      mu ~ normal(0, 5); tau ~ cauchy(0, 5); eta ~ std_normal();
      y ~ normal(theta, sigma);
    }
    generated quantities {
      real yrep = normal_rng(theta[1], sigma[1]);
      vector[J] log_lik;
      for (j in 1:J) log_lik[j] = normal_lpdf(y[j] | theta[j], sigma[j]);
    }", data = list(J = 8L, y = c(28, 8, -3, 7, -1, 1, 18, 12),
                    sigma = c(15, 10, 16, 11, 9, 11, 10, 18)), ...)
}

skip_without_nutpie <- function() {
  skip_if_not_installed("nutpieR")
  skip_without_runtime()
}

nutpie_fit <- function(model = nutpie_eight_schools(), ...) {
  sample_nutpie(model, chains = 2, warmup = 200, samples = 100, seed = 5, ...)
}

test_that("sample_nutpie names the install when nutpieR is missing", {
  local_mocked_bindings(nutpie_installed = function() FALSE, .package = "stanli")
  expect_error(sample_nutpie(NULL), "nutpieR.*install\\.packages.*andytimm")
})

test_that("sample_nutpie rejects arguments it sets itself", {
  skip_without_nutpie()
  model <- nutpie_eight_schools()
  for (name in c("data", "num_draws", "store_unconstrained", "pars"))
    expect_error(do.call(sample_nutpie, c(list(model), setNames(list(1), name))),
                 name)
  expect_error(sample_nutpie(list()), "stanli_model")
})

test_that("sample_nutpie returns a stanli_fit shaped like sample_model's", {
  skip_without_nutpie()
  model <- nutpie_eight_schools()
  fit <- nutpie_fit(model)
  expect_s3_class(fit, "stanli_fit")
  expect_identical(dim(fit$draws), c(100L, 2L, length(model$columns)))
  expect_identical(dimnames(fit$draws)[[3L]], model$columns)
  expect_identical(fit$columns, model$columns)
  expect_identical(dimnames(fit$sampler)[[3L]],
                   .Call("stanli_r_sampler_columns"))
  expect_identical(dim(fit$sampler), c(100L, 2L, 7L))
  expect_identical(dim(fit$unconstrained), c(100L, 2L, model$n_unconstrained))
  expect_true(all(is.finite(fit$draws)))
  expect_identical(fit$chains, 2L)
  expect_identical(fit$samples, 100L)
  expect_identical(fit$warmup, 200L)
  expect_identical(fit$thin, 1L)
  expect_false(fit$save_warmup)
  expect_identical(fit$warmup_draws, 0L)
  expect_identical(fit$max_depth, 10L)
  expect_identical(fit$sampler_backend, "nutpie")
  expect_identical(fit$adaptation, "diag")
  expect_null(fit$metric)
  expect_null(fit$inv_metric)
  expect_length(fit$step_size, 2L)
  expect_true(all(fit$step_size > 0))
  expect_length(fit$report$n_divergent, 2L)
  expect_length(fit$report$n_max_treedepth, 2L)
  expect_true(all(fit$report$sampling_seconds >= 0))
})

test_that("the sampler lp__ is the log density at the unconstrained draw", {
  skip_without_nutpie()
  model <- nutpie_eight_schools()
  fit <- nutpie_fit(model)
  for (i in c(1L, 50L, 100L)) for (chain in 1:2) {
    expect_equal(unname(fit$sampler[i, chain, "lp__"]),
                 log_prob_grad(fit$model, fit$unconstrained[i, chain, ])$lp,
                 tolerance = 1e-12)
  }
})

test_that("constrained draws are the constraint of the unconstrained ones", {
  skip_without_nutpie()
  fit <- nutpie_fit()
  expect_true(all(fit$draws[, , "tau"] > 0))
  expect_equal(fit$draws[, , "tau"], exp(fit$unconstrained[, , 2L]),
               tolerance = 1e-12)
  expect_equal(fit$draws[, , "mu"], fit$unconstrained[, , 1L],
               tolerance = 1e-12)
})

test_that("posterior means agree with stanli's own sampler", {
  skip_without_nutpie()
  model <- nutpie_eight_schools()
  ours <- sample_model(model, chains = 4, seed = 11, warmup = 1000,
                       samples = 2000, refresh = 0)
  for (adaptation in c("diag", "low_rank")) {
    theirs <- sample_nutpie(model, chains = 4, seed = 11, warmup = 1000,
                            samples = 2000, adaptation = adaptation)
    expect_identical(theirs$adaptation, adaptation)
    a <- summary(ours)
    b <- summary(theirs)
    for (v in c("mu", "tau", "theta[1]", "yrep")) {
      i <- match(v, a$variable)
      se <- sqrt(a$mcse_mean[i]^2 + b$mcse_mean[i]^2)
      expect_lt(abs(a$mean[i] - b$mean[i]), 5 * se, label = paste(adaptation, v))
    }
    expect_lt(max(b$rhat), 1.05)
  }
})

test_that("save_warmup puts the warmup draws first", {
  skip_without_nutpie()
  model <- nutpie_eight_schools()
  fit <- sample_nutpie(model, chains = 2, warmup = 60, samples = 40, seed = 5,
                       save_warmup = TRUE)
  expect_true(fit$save_warmup)
  expect_identical(fit$warmup_draws, 60L)
  expect_identical(dim(fit$draws), c(100L, 2L, length(model$columns)))
  expect_identical(dim(fit$sampler), c(100L, 2L, 7L))
  expect_identical(dim(fit$unconstrained), c(100L, 2L, model$n_unconstrained))
  raw <- nutpieR::nutpie_sample(stanli_runtime_path(),
                                data = bridgestan_data(model), num_draws = 40,
                                num_warmup = 60, num_chains = 2, seed = 5,
                                save_warmup = TRUE, progress = "none")
  expect_identical(unname(unclass(fit$draws[1:60, , "mu"])),
                   unname(unclass(attr(raw, "warmup_draws")[, , "mu"])))
  expect_identical(unname(unclass(fit$draws[61:100, , "mu"])),
                   unname(unclass(raw[, , "mu"])))
  expect_identical(unname(dim(as_draws_array(fit))[1L]), 40L)
  expect_identical(unname(dim(as_draws_array(fit, inc_warmup = TRUE))[1L]), 100L)
  expect_true(all(fit$sampler[1:60, , "stepsize__"] > 0))
})

test_that("a model without data samples", {
  skip_without_nutpie()
  model <- stanli_model(code = "parameters { real x; } model { x ~ normal(1, 2); }")
  fit <- sample_nutpie(model, chains = 2, warmup = 300, samples = 1000)
  s <- summary(fit)
  expect_equal(s$mean[s$variable == "x"], 1, tolerance = 0.3)
})

test_that("MIR models sample: fast_math and includes", {
  skip_without_nutpie()
  data <- list(N = 4L, x = c(1, 2, 3, 4), y = c(1.1, 1.9, 3.2, 3.9))
  code <- "
    data { int<lower=0> N; vector[N] x; vector[N] y; }
    parameters { real a; real b; real<lower=0> sigma; }
    model { y ~ normal(a + b * x, sigma); }"
  fast <- stanli_model(code = code, data = data, fast_math = TRUE)
  expect_true(fast$source$is_mir)
  fit <- sample_nutpie(fast, chains = 2, warmup = 200, samples = 100)
  expect_identical(dim(fit$draws), c(100L, 2L, 3L))

  root <- tempfile("nutpie includes ")
  dir.create(root)
  on.exit(unlink(root, recursive = TRUE), add = TRUE)
  writeLines("a ~ normal(0, 10); b ~ normal(0, 10);", file.path(root, "prior.stan"))
  included <- stanli_model(
    code = sub("model {", "model {\n#include prior.stan", code, fixed = TRUE),
    data = data, include_paths = root)
  expect_true(included$source$is_mir)
  fit <- sample_nutpie(included, chains = 2, warmup = 200, samples = 100)
  expect_identical(dim(fit$draws), c(100L, 2L, 3L))
})

test_that("transformed data follows the run seed", {
  skip_without_nutpie()
  model <- stanli_model(code = "
    transformed data { real shift = normal_rng(0, 10); }
    parameters { real x; }
    model { x ~ normal(shift, 0.01); }
    generated quantities { real the_shift = shift; }")
  fit <- sample_nutpie(model, chains = 2, warmup = 100, samples = 20, seed = 17)
  ours <- sample_model(model, chains = 1, warmup = 50, samples = 5, seed = 17,
                       refresh = 0)
  expect_identical(fit$model$seed, 17)
  expect_equal(unique(as.vector(fit$draws[, , "the_shift"])),
               unique(as.vector(ours$draws[, , "the_shift"])))
  other <- sample_nutpie(model, chains = 1, warmup = 100, samples = 5, seed = 18)
  expect_false(isTRUE(all.equal(other$draws[1, 1, "the_shift"],
                                fit$draws[1, 1, "the_shift"])))
})

test_that("a nutpie fit works with the stanli consumers", {
  skip_without_nutpie()
  fit <- nutpie_fit()
  s <- summary(fit)
  expect_identical(s$variable, fit$columns)
  expect_true(all(is.finite(s$rhat)))
  out <- capture.output(stanli_diagnose(fit))
  expect_true(length(out) > 0L)
  draws <- as_draws_array(fit)
  expect_identical(unname(dim(draws)), c(100L, 2L, length(fit$columns)))
  expect_identical(unname(dim(log_lik(fit))), c(100L, 2L, 8L))
  expect_output(print(fit), "stanli fit")
})

test_that("a nutpie fit converts to the other fit classes", {
  skip_without_nutpie()
  fit <- nutpie_fit()
  cmd <- as_cstanfit(fit)
  expect_identical(dim(cmd$draws()), c(100L, 2L, length(fit$columns) + 1L))
  expect_identical(dim(cmd$sampler_diagnostics()), c(100L, 2L, 6L))
  expect_identical(cmd$metadata()$iter_sampling, 100L)
  expect_error(cmd$inv_metric(), "nutpie")
  rfit <- as_rfit(fit)
  expect_identical(dim(get_sampler_params(rfit)[[1L]]), c(100L, 6L))
  expect_identical(dim(get_elapsed_time(rfit)), c(2L, 2L))
  skip_if_not_installed("rstan")
  sf <- as_stanfit(fit)
  expect_identical(sf@sim$chains, 2L)
})

test_that("a nutpie fit does not offer an inverse metric for reuse", {
  skip_without_nutpie()
  fit <- nutpie_fit()
  model <- nutpie_eight_schools()
  again <- sample_model(model, chains = 1, warmup = 50, samples = 20,
                        refresh = 0, inv_metric = fit$inv_metric,
                        step_size = mean(fit$step_size), adapt_engaged = FALSE)
  expect_s3_class(again, "stanli_fit")
})
