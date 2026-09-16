test_that("live methods match the independently recorded RStan oracle", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  reference <- dget(test_path("fixtures", "stanfit-live-reference.R"))
  code <- paste(readLines(test_path("fixtures", "stanfit-live.stan")), collapse = "\n")
  expect_identical(code, reference$code)
  model <- stanli_model(code = code)
  fit <- sample_model(model, chains = 1, warmup = 50, samples = 50, refresh = 0)
  sf <- as_stanfit(fit)
  expect_s4_class(sf, "stanli_stanfit")
  expect_true(inherits(sf, "stanfit"))
  expect_false(rstan:::is_sfinstance_valid(sf))
  expect_identical(rstan::get_num_upars(sf), reference$n)
  # RStan fixture values are printed to 15 significant digits. Different Stan
  # Math builds may also round elementary operations differently: 1e-10 is the
  # numerical tolerance, while container dimensions/names are checked exactly.
  for (point in reference$points) {
    expect_equal(rstan::log_prob(sf, point$q), as.numeric(point$lp), tolerance = 1e-10)
    expect_equal(rstan::log_prob(sf, point$q, gradient = TRUE), point$lp,
                 tolerance = 1e-10)
    expect_equal(rstan::grad_log_prob(sf, point$q), point$grad, tolerance = 1e-10)
    constrained <- rstan::constrain_pars(sf, point$q)
    expect_identical(names(constrained), names(point$constrained))
    expect_identical(lapply(constrained, dim), lapply(point$constrained, dim))
    expect_equal(constrained, point$constrained, tolerance = 1e-10)
    expect_equal(rstan::unconstrain_pars(sf, constrained), point$unconstrained,
                 tolerance = 1e-10)
    expect_equal(rstan::unconstrain_pars(sf, constrained), point$q, tolerance = 1e-10)
  }
  # The retained model owns the native handle after the source fit is released.
  rm(model, fit)
  gc()
  expect_equal(rstan::log_prob(sf, reference$points[[2]]$q),
               as.numeric(reference$points[[2]]$lp), tolerance = 1e-10)
})

test_that("live methods validate flags, coordinates, and model attachment", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  model <- stanli_model(code = "parameters { real<lower=0> sigma; }
                                model { sigma ~ normal(0, 1); }")
  fit <- sample_model(model, chains = 1, warmup = 50, samples = 50, refresh = 0)
  sf <- as_stanfit(fit)
  expect_equal(rstan::log_prob(sf, 0.2), -0.5 * exp(0.4) + 0.2)
  expect_equal(as.numeric(rstan::grad_log_prob(sf, 0.2)), 1 - exp(0.4))
  expect_error(rstan::log_prob(sf, 0, adjust_transform = FALSE), "Jacobian")
  expect_error(rstan::grad_log_prob(sf, 0, adjust_transform = FALSE), "Jacobian")
  expect_error(rstan::log_prob(sf, 0, gradient = NA), "gradient must be")
  expect_error(rstan::log_prob(sf, 0, adjust_transform = c(TRUE, FALSE)),
               "adjust_transform must be")
  for (q in list(numeric(), c(0, 1), NA_real_, Inf, "0")) {
    expect_error(rstan::log_prob(sf, q), "finite numeric unconstrained")
    expect_error(rstan::constrain_pars(sf, q), "finite numeric unconstrained")
  }
  expect_error(rstan::unconstrain_pars(sf, list(1)), "unique parameter names")
  expect_error(rstan::unconstrain_pars(sf, list(sigma = -1)), "sigma")
  expect_error(rstan::unconstrain_pars(sf, list(other = 1)), "sigma")
  expect_error(as_stanfit(fit, model = es_model()), "model must match")
  expect_error(rstan::log_prob(as_stanfit(fit, model = NULL), 0), "no live stanli")
  restored <- unserialize(serialize(fit, NULL))
  expect_error(rstan::log_prob(as_stanfit(restored), 0), "no live stanli")
  reattached <- as_stanfit(restored, model = model)
  expect_identical(rstan::log_prob(reattached, 0.2), rstan::log_prob(sf, 0.2))
  saved_sf <- unserialize(serialize(sf, NULL))
  reattached_sf <- as_stanfit(saved_sf, model = model)
  expect_identical(as.array(reattached_sf), as.array(saved_sf))
  expect_identical(rstan::log_prob(reattached_sf, 0.2), rstan::log_prob(sf, 0.2))
  expect_error(rstan::log_prob(saved_sf, 0.2), "no live stanli")
  expect_identical(as_stanfit(sf), sf)
  expect_error(as_stanfit(sf, model = es_model()), "model must match")
  testthat::local_mocked_bindings(stanli_exact_lp = function() FALSE, .package = "stanli")
  expect_error(rstan::log_prob(sf, 0), "exact-lp runtime")
  expect_error(rstan::grad_log_prob(sf, 0), "exact-lp runtime")
})

test_that("a fresh session restores subclass draws and reports a missing handle", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("callr")
  fit <- sample_model(es_model(), chains = 2, warmup = 50, samples = 50, refresh = 0)
  sf <- as_stanfit(fit)
  file <- tempfile(fileext = ".rds")
  on.exit(unlink(file))
  saveRDS(sf, file)
  restored <- callr::r(function(file) {
    sf <- readRDS(file)  # No explicit library(stanli) or library(rstan).
    list(valid = methods::validObject(sf), draws = as.array(sf),
         n = rstan::get_num_upars(sf),
         error = tryCatch(rstan::log_prob(sf, rep(0, 10)), error = conditionMessage))
  }, list(file), libpath = .libPaths(), env = c(STANLI_RUNTIME = tempfile()))
  expect_true(restored$valid)
  expect_identical(restored$draws, as.array(sf))
  expect_identical(restored$n, 10L)
  expect_match(restored$error, "no live stanli model")
})

test_that("LOO moment matching uses the live density and transforms", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("loo")
  y <- c(0, 0.1, -0.1, 0.2, 7)
  model <- stanli_model(code = "
    data { int N; vector[N] y; }
    parameters { real mu; }
    model { mu ~ normal(0, 2); y ~ normal(mu, 1); }
    generated quantities {
      vector[N] log_lik;
      for (i in 1:N) log_lik[i] = normal_lpdf(y[i] | mu, 1);
    }", data = list(N = length(y), y = y))
  fit <- sample_model(model, chains = 4, warmup = 500, samples = 1000,
                      seed = 42, refresh = 0)
  sf <- as_stanfit(fit)
  expect_warning(initial <- loo::loo(sf, cores = 1), "Pareto k diagnostic values")
  expect_gt(initial$diagnostics$pareto_k[5], 0.5)
  matched <- loo::loo_moment_match(sf, loo = initial, cores = 1, k_threshold = 0.5)
  expect_s3_class(matched, "psis_loo")
  expect_lt(matched$diagnostics$pareto_k[5], 0.5)
  # Analytic leave-one-out predictive density for the normal-normal model.
  # This is a Monte Carlo check (0.2 log units), distinct from the deterministic
  # 1e-10 density/transform oracle above.
  variance <- 1 / (1 / 4 + length(y) - 1)
  means <- (sum(y) - y) * variance
  exact <- sum(stats::dnorm(y, means, sqrt(1 + variance), log = TRUE))
  expect_lt(abs(matched$estimates["elpd_loo", "Estimate"] - exact), 0.2)
})

test_that("forward transforms execute random GQ and inverse transforms ignore them", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  model <- stanli_model(code = "parameters { real a; } model { a ~ std_normal(); }
    generated quantities { real y_rep = normal_rng(a, 1); }")
  sf <- as_stanfit(sample_model(model, chains = 1, warmup = 20, samples = 20,
                               refresh = 0))
  first <- rstan::constrain_pars(sf, 0)
  second <- rstan::constrain_pars(sf, 0)
  expect_identical(first$a, 0)
  expect_true(is.finite(first$y_rep))
  expect_false(identical(first$y_rep, second$y_rep))
  expect_identical(rstan::unconstrain_pars(sf, first), 0)
})
