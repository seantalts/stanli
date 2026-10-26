normal_code <- "
  data { int<lower=0> N; vector[N] y; }
  parameters { real mu; real<lower=0> sigma; }
  model { mu ~ normal(0, 5); sigma ~ normal(0, 3); y ~ normal(mu, sigma); }
  generated quantities { real ytilde = normal_rng(mu, sigma); }"

normal_data <- function() list(N = 10L, y = c(1.2, 0.4, 2.1, 1.7, 0.9, 1.5, 2.4, 0.2, 1.1, 1.8))

stanmodel_fixture <- function() {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  as_stanmodel(normal_code, model_name = "normal_fixture")
}

draws_of <- function(fit, ...) rstan::extract(fit, permuted = FALSE, ...)

test_that("as_stanmodel accepts code, a file, and an rstan stanmodel", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  m <- as_stanmodel(normal_code)
  expect_s4_class(m, "stanli_stanmodel")
  expect_true(methods::is(m, "stanmodel"))
  expect_identical(m@model_code, normal_code)
  expect_identical(m@model_name, "anon_model")
  expect_identical(as_stanmodel(normal_code, model_name = "named")@model_name, "named")

  path <- file.path(tempdir(), "from_file.stan")
  writeLines(normal_code, path)
  on.exit(unlink(path))
  from_file <- as_stanmodel(path)
  expect_identical(from_file@model_name, "from_file")
  expect_match(from_file@model_code, "parameters", fixed = TRUE)
  expect_identical(as_stanmodel(path, model_name = "other")@model_name, "other")

  compiled <- methods::new("stanmodel", model_name = "from_rstan", model_code = normal_code)
  converted <- as_stanmodel(compiled)
  expect_s4_class(converted, "stanli_stanmodel")
  expect_identical(converted@model_name, "from_rstan")
  expect_identical(converted@model_code, normal_code)
  expect_identical(as_stanmodel(converted), converted)

  expect_error(as_stanmodel("parameters { real mu; } model { mu ~ normal(0, ; }"),
               "syntax|error|parse", ignore.case = TRUE)
  expect_error(as_stanmodel(42), "single string|stanmodel")
  expect_error(as_stanmodel(methods::new("stanmodel", model_name = "empty")), "model_code")
})

test_that("sampling runs on a stanli_stanmodel and returns a working stanfit", {
  m <- stanmodel_fixture()
  fit <- rstan::sampling(m, data = normal_data(), chains = 2, iter = 200,
                         warmup = 100, seed = 11, refresh = 0)
  expect_s4_class(fit, "stanli_stanfit")
  expect_identical(fit@model_name, "normal_fixture")
  expect_identical(fit@mode, 0L)

  draws <- draws_of(fit)
  expect_identical(dim(draws), c(100L, 2L, 4L))
  expect_identical(dimnames(draws)$parameters, c("mu", "sigma", "ytilde", "lp__"))
  expect_equal(mean(draws[, , "mu"]), mean(normal_data()$y), tolerance = 0.3)

  expect_identical(dim(rstan::summary(fit)$summary)[1L], 4L)
  params <- rstan::get_sampler_params(fit, inc_warmup = FALSE)
  expect_length(params, 2L)
  expect_identical(colnames(params[[1L]])[1:2], c("accept_stat__", "treedepth__"))
  expect_identical(nrow(params[[1L]]), 100L)
  expect_identical(length(rstan::extract(fit)$mu), 200L)

  reference <- stanli_model(code = normal_code, data = normal_data())
  q <- c(0.3, -0.4)
  expect_equal(rstan::log_prob(fit, q), log_prob_grad(reference, q)$lp, tolerance = 1e-10)
  expect_equal(as.numeric(rstan::grad_log_prob(fit, q)), log_prob_grad(reference, q)$grad,
               tolerance = 1e-10)
})

test_that("iter, warmup, thin, chains, seed and save_warmup map as in rstan", {
  m <- stanmodel_fixture()
  fit <- rstan::sampling(m, data = normal_data(), chains = 3, iter = 100,
                         warmup = 40, thin = 3, seed = 4, refresh = 0)
  expect_identical(dim(draws_of(fit))[1:2], c(20L, 3L))
  expect_identical(dim(draws_of(fit, inc_warmup = TRUE))[1L], 34L)
  expect_identical(fit@sim$iter, 100L)
  expect_identical(fit@sim$warmup, 40L)
  expect_identical(fit@sim$thin, 3L)

  no_warmup <- rstan::sampling(m, data = normal_data(), chains = 1, iter = 100,
                               warmup = 40, seed = 4, refresh = 0, save_warmup = FALSE)
  expect_identical(dim(draws_of(no_warmup, inc_warmup = TRUE))[1L], 60L)

  default_warmup <- rstan::sampling(m, data = normal_data(), chains = 1, iter = 120,
                                    seed = 4, refresh = 0)
  expect_identical(dim(draws_of(default_warmup))[1L], 60L)

  again <- rstan::sampling(m, data = normal_data(), chains = 3, iter = 100,
                           warmup = 40, thin = 3, seed = 4, refresh = 0)
  expect_identical(draws_of(again), draws_of(fit))
  other <- rstan::sampling(m, data = normal_data(), chains = 3, iter = 100,
                           warmup = 40, thin = 3, seed = 5, refresh = 0)
  expect_false(identical(draws_of(other), draws_of(fit)))

  expect_error(rstan::sampling(m, data = normal_data(), iter = 0, refresh = 0), "iter")
  expect_error(rstan::sampling(m, data = normal_data(), iter = 10, warmup = 10,
                               refresh = 0), "warmup")
  expect_error(rstan::sampling(m, data = normal_data(), iter = 10, thin = 11,
                               refresh = 0), "thin")
})

test_that("chain_id selects the chain streams", {
  m <- stanmodel_fixture()
  run <- function(...) rstan::sampling(m, data = normal_data(), iter = 80, warmup = 40,
                                       seed = 9, refresh = 0, ...)
  pair <- run(chains = 2, chain_id = 3)
  expect_identical(vapply(pair@stan_args, function(a) a$chain_id, integer(1)), 3:4)
  single <- run(chains = 1, chain_id = 4)
  expect_identical(draws_of(single)[, 1, ], draws_of(pair)[, 2, ])
  expect_error(run(chains = 2, chain_id = c(1, 3)), "chain_id")
})

test_that("pars and include choose the stored variables", {
  m <- stanmodel_fixture()
  run <- function(...) rstan::sampling(m, data = normal_data(), chains = 1, iter = 60,
                                       warmup = 30, seed = 2, refresh = 0, ...)
  expect_identical(dimnames(draws_of(run(pars = "mu")))$parameters, c("mu", "lp__"))
  expect_identical(dimnames(draws_of(run(pars = c("mu", "sigma"), include = FALSE)))$parameters,
                   c("ytilde", "lp__"))
  expect_identical(dimnames(draws_of(run(pars = list("sigma"))))$parameters, c("sigma", "lp__"))
  expect_identical(dimnames(draws_of(run(include = FALSE)))$parameters, "lp__")
  expect_error(run(pars = "nonesuch"), "nonesuch")
  expect_equal(rstan::log_prob(run(pars = "mu"), c(0, 0)),
               rstan::log_prob(run(), c(0, 0)))
})

test_that("control maps to stanli arguments and rejects the rest by name", {
  m <- stanmodel_fixture()
  run <- function(...) rstan::sampling(m, data = normal_data(), chains = 1, iter = 160,
                                       warmup = 80, seed = 6, refresh = 0, ...)
  fit <- run(control = list(adapt_delta = 0.95, max_treedepth = 3))
  control <- fit@stan_args[[1L]]$control
  expect_equal(control$adapt_delta, 0.95)
  expect_equal(control$max_treedepth, 3)
  expect_true(all(rstan::get_sampler_params(fit)[[1L]][, "treedepth__"] <= 3))

  unit <- run(control = list(metric = "unit_e", stepsize = 0.3, adapt_engaged = FALSE))
  expect_identical(unit@stan_args[[1L]]$sampler_t, "NUTS(unit_e)")
  expect_equal(unit@stan_args[[1L]]$control$stepsize, 0.3)
  expect_true(all(abs(rstan::get_sampler_params(unit)[[1L]][, "stepsize__"] - 0.3) < 1e-12))

  expect_error(run(control = list(adapt_gamma = 0.1)), "adapt_gamma")
  expect_error(run(control = list(adapt_delta = 0.9, int_time = 1, epsilon = 0.1)),
               "int_time, epsilon")
  expect_error(run(control = list(metric = "bogus")), "metric")
})

test_that("algorithm, files and unknown arguments are handled explicitly", {
  m <- stanmodel_fixture()
  run <- function(...) rstan::sampling(m, data = normal_data(), chains = 1, iter = 40,
                                       warmup = 20, seed = 1, refresh = 0, ...)
  expect_error(run(algorithm = "HMC"), "HMC")
  expect_error(run(sample_file = tempfile()), "sample_file")
  expect_error(run(diagnostic_file = tempfile()), "diagnostic_file")
  expect_error(run(not_an_argument = 1), "not_an_argument")
  expect_error(run(test_grad = TRUE), "test_grad")
  expect_s4_class(run(sample_file = NULL, diagnostic_file = NULL, verbose = FALSE,
                      check_data = TRUE, open_progress = FALSE, show_messages = FALSE,
                      cores = 1), "stanli_stanfit")
})

test_that("cores bounds the chains run at once without changing the draws", {
  m <- stanmodel_fixture()
  run <- function(cores) rstan::sampling(m, data = normal_data(), chains = 3, iter = 80,
                                         warmup = 40, seed = 7, refresh = 0, cores = cores)
  expect_identical(draws_of(run(1)), draws_of(run(3)))
})

test_that("Fixed_param runs generated quantities only", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  m <- as_stanmodel("generated quantities { real z = normal_rng(0, 1); }")
  fit <- rstan::sampling(m, algorithm = "Fixed_param", chains = 2, iter = 50,
                         warmup = 0, seed = 3, refresh = 0)
  expect_identical(dim(draws_of(fit)), c(50L, 2L, 2L))
  expect_identical(fit@stan_args[[1L]]$algorithm, "Fixed_param")
  expect_gt(stats::sd(draws_of(fit)[, , "z"]), 0)
  ignored <- rstan::sampling(m, algorithm = "Fixed_param", chains = 1, iter = 20,
                             warmup = 0, seed = 3, refresh = 0,
                             control = list(adapt_delta = 0.9))
  expect_identical(dim(draws_of(ignored))[1L], 20L)
})

test_that("chains = 0 gives rstan's empty fit", {
  m <- stanmodel_fixture()
  expect_message(
    fit <- rstan::sampling(m, data = normal_data(), chains = 0, refresh = 0),
    "chains is less than 1")
  expect_s4_class(fit, "stanfit")
  expect_identical(fit@mode, 2L)
  expect_identical(fit@sim, list())
  expect_identical(fit@model_pars, c("mu", "sigma", "ytilde", "lp__"))
  expect_identical(fit@par_dims$mu, integer(0))
  expect_identical(fit@model_name, "normal_fixture")
  expect_identical(fit@stan_args, list())
})

test_that("the caller's random stream is untouched", {
  m <- stanmodel_fixture()
  set.seed(42)
  before <- random_state()
  rstan::sampling(m, data = normal_data(), chains = 2, iter = 40, warmup = 20,
                  seed = 8, refresh = 0, init = list(mu = 0.5))
  expect_identical(random_state(), before)
  without_random_state({
    rstan::sampling(m, data = normal_data(), chains = 1, iter = 40, warmup = 20,
                    seed = 8, refresh = 0, init = function() list(mu = 0.5))
    expect_null(random_state())
  })
})

test_that("the default seed is drawn from the caller's stream as rstan does", {
  m <- stanmodel_fixture()
  run <- function(...) rstan::sampling(m, data = normal_data(), chains = 1, iter = 60,
                                       warmup = 30, refresh = 0, ...)
  set.seed(10)
  implicit <- run()
  set.seed(10)
  explicit <- run(seed = sample.int(.Machine$integer.max, 1))
  expect_identical(draws_of(implicit), draws_of(explicit))
  expect_identical(implicit@stan_args[[1L]]$seed, explicit@stan_args[[1L]]$seed)
})

test_that("data may be named variables in the calling frame", {
  m <- stanmodel_fixture()
  fit <- local({
    N <- 10L
    y <- normal_data()$y
    rstan::sampling(m, data = c("N", "y"), chains = 1, iter = 40, warmup = 20,
                    seed = 1, refresh = 0)
  })
  expect_s4_class(fit, "stanli_stanfit")
})

test_that("rstan_init translates the scalar forms", {
  skip_without_runtime()
  model <- stanli_model(code = normal_code, data = normal_data())
  translate <- function(init, ...) stanli:::rstan_init(model, init, chains = 2, ...)
  expect_identical(translate("random"), list(init = NULL, init_radius = 2))
  expect_identical(translate("random", init_r = 3)$init_radius, 3)
  expect_identical(translate("0")$init_radius, 0)
  expect_identical(translate(0)$init_radius, 0)
  expect_identical(translate(0.5)$init_radius, 0.5)
  expect_identical(translate(0.5)$init, NULL)
  expect_error(translate("nonsense"), "init")
  expect_error(translate(-1), "init")
  expect_error(translate(NULL), "init")
})

test_that("rstan_init builds starts from lists and functions", {
  skip_without_runtime()
  model <- stanli_model(code = normal_code, data = normal_data())
  translate <- function(init, chains = 2L, ...)
    stanli:::rstan_init(model, init, chains = chains, seed = 3, ...)

  full <- translate(list(mu = 1.5, sigma = 2))
  expect_identical(dim(full$init), c(2L, 2L))
  expect_equal(unname(full$init[1, ]), c(1.5, log(2)))
  expect_equal(full$init[1, ], full$init[2, ])

  unknown <- translate(list(mu = 1.5, sigma = 2, junk = 7, ytilde = 3))
  expect_equal(unknown$init, full$init)

  per_chain <- translate(list(list(mu = 1, sigma = 1), list(mu = 2, sigma = 4)))
  expect_equal(unname(per_chain$init[, 1]), c(1, 2))
  expect_equal(unname(per_chain$init[, 2]), log(c(1, 4)))
  expect_error(translate(list(list(mu = 1), list(mu = 2), list(mu = 3))), "3 sets.*2 chains")

  partial <- translate(list(mu = 0.25), init_r = 1)
  expect_equal(unname(partial$init[, 1]), c(0.25, 0.25))
  expect_true(all(abs(partial$init[, 2]) <= 1))
  expect_false(partial$init[1, 2] == partial$init[2, 2])
  expect_identical(translate(list(mu = 0.25), init_r = 1)$init, partial$init)

  empty <- translate(list(list(), list()))
  expect_identical(dim(empty$init), c(2L, 2L))
  expect_true(all(abs(empty$init) <= 2))

  with_id <- translate(function(chain_id) list(mu = chain_id, sigma = 1), chains = 3L)
  expect_equal(unname(with_id$init[, 1]), 1:3)
  shifted <- stanli:::rstan_init(model, function(chain_id) list(mu = chain_id, sigma = 1),
                                 chains = 2L, seed = 3, chain_id = 5L)
  expect_equal(unname(shifted$init[, 1]), 5:6)
  without_id <- translate(function() list(mu = -1, sigma = 1))
  expect_equal(unname(without_id$init[, 1]), c(-1, -1))
  expect_error(translate(function() 3), "named list")

  expect_error(translate(list(mu = 1, sigma = -1)), "No usable starting values")
})

test_that("rstan_init leaves the caller's random stream alone", {
  skip_without_runtime()
  model <- stanli_model(code = normal_code, data = normal_data())
  set.seed(77)
  before <- random_state()
  stanli:::rstan_init(model, list(mu = 1), chains = 3, seed = 5)
  expect_identical(random_state(), before)
  without_random_state({
    stanli:::rstan_init(model, list(mu = 1), chains = 3, seed = 5)
    expect_null(random_state())
  })
})

test_that("rstan_init retries random completions until the density is finite", {
  skip_without_runtime()
  model <- stanli_model(code = "
    parameters { real x; real y; }
    model {
      if (y > -1.4) target += negative_infinity();
      else target += -0.5 * (x^2 + y^2);
    }")
  starts <- stanli:::rstan_init(model, list(x = 0.5), chains = 6, seed = 12)$init
  expect_identical(dim(starts), c(6L, 2L))
  for (chain in 1:6) {
    value <- log_prob_grad(model, starts[chain, ])
    expect_true(is.finite(value$lp) && all(is.finite(value$grad)))
  }
  expect_equal(unname(starts[, 1]), rep(0.5, 6))

  hopeless <- stanli_model(code = "parameters { real x; } model { target += negative_infinity(); }")
  expect_error(stanli:::rstan_init(hopeless, function() list(), chains = 1, seed = 1),
               "No usable starting values in 100 attempts")
  expect_error(stanli:::rstan_init(hopeless, list(list(x = 1)), chains = 1, seed = 1),
               "No usable starting values in 1 attempt")
})

test_that("sampling accepts every rstan init form", {
  m <- stanmodel_fixture()
  run <- function(init) rstan::sampling(m, data = normal_data(), chains = 2, iter = 60,
                                        warmup = 30, seed = 3, refresh = 0, init = init)
  for (init in list("random", "0", 0, 0.5, list(mu = 1, nothing = 2),
                    list(list(mu = 1), list(mu = 2, sigma = 1)),
                    function() list(sigma = 1.5),
                    function(chain_id) list(mu = chain_id)))
    expect_s4_class(run(init), "stanli_stanfit")
  expect_error(run(list(list(mu = 1))), "1 sets.*2 chains")
  expect_error(run("garbage"), "init")
})

test_that("starting values change where the chain begins", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  m <- as_stanmodel("parameters { real mu; } model { mu ~ normal(0, 1); }")
  first <- function(init) {
    fit <- rstan::sampling(m, chains = 1, iter = 2, warmup = 0, seed = 1, refresh = 0,
                           init = init, control = list(adapt_engaged = FALSE, stepsize = 1e-6))
    draws_of(fit)[1, 1, "mu"]
  }
  expect_equal(unname(first(list(mu = 4))), 4, tolerance = 1e-3)
  expect_equal(unname(first(list(mu = -3))), -3, tolerance = 1e-3)
})

test_that("vb runs ADVI and returns a stanfit", {
  m <- stanmodel_fixture()
  fit <- suppressWarnings(rstan::vb(m, data = normal_data(), seed = 3, output_samples = 200,
                                    iter = 3000, refresh = 0))
  expect_s4_class(fit, "stanli_stanfit")
  expect_identical(fit@stan_args[[1L]]$method, "variational")
  expect_identical(fit@stan_args[[1L]]$algorithm, "meanfield")
  expect_identical(length(rstan::extract(fit)$mu), 200L)
  expect_identical(fit@model_name, "normal_fixture")
  expect_equal(mean(rstan::extract(fit)$mu), mean(normal_data()$y), tolerance = 0.5)

  full <- suppressWarnings(rstan::vb(m, data = normal_data(), seed = 3, algorithm = "fullrank",
                                     output_samples = 50, iter = 3000, refresh = 0,
                                     tol_rel_obj = 0.05, eta = 0.5, adapt_engaged = FALSE,
                                     grad_samples = 2, elbo_samples = 50, eval_elbo = 50))
  expect_identical(full@stan_args[[1L]]$algorithm, "fullrank")
  expect_identical(length(rstan::extract(full)$mu), 50L)

  excluded <- suppressWarnings(rstan::vb(m, data = normal_data(), seed = 3, pars = "mu",
                                         output_samples = 20, iter = 2000, refresh = 0))
  expect_identical(excluded@sim$fnames_oi, c("mu", "lp__", "lp_approx__"))

  expect_error(rstan::vb(m, data = normal_data(), importance_resampling = TRUE), "importance")
  expect_error(rstan::vb(m, data = normal_data(), keep_every = 2), "keep_every")
  expect_error(rstan::vb(m, data = normal_data(), sample_file = tempfile()), "sample_file")
  expect_error(rstan::vb(m, data = normal_data(), not_an_argument = 1), "not_an_argument")
})

test_that("optimizing stops and names the alternative", {
  m <- stanmodel_fixture()
  expect_error(rstan::optimizing(m, data = normal_data()), "optimize_model")
  expect_error(rstan::optimizing(m, data = normal_data()), "Jacobian")
})

test_that("as_stanfit(plain = TRUE) gives a bare stanfit", {
  m <- stanmodel_fixture()
  fit <- rstan::sampling(m, data = normal_data(), chains = 2, iter = 60, warmup = 30,
                         seed = 1, refresh = 0)
  plain <- as_stanfit(fit, plain = TRUE)
  expect_identical(as.vector(class(plain)), "stanfit")
  expect_true(class(plain) == "stanfit")
  expect_true("stanfit" %in% class(plain))
  expect_false(methods::is(plain, "stanli_stanfit"))
  expect_identical(draws_of(plain), draws_of(fit))
  expect_identical(rstan::summary(plain)$summary, rstan::summary(fit)$summary)
  expect_error(rstan::log_prob(plain, c(0, 0)))

  keep <- as_stanfit(fit)
  expect_s4_class(keep, "stanli_stanfit")

  native <- sample_model(stanli_model(code = normal_code, data = normal_data()),
                         chains = 1, warmup = 30, samples = 30, refresh = 0)
  expect_identical(as.vector(class(as_stanfit(native, plain = TRUE))), "stanfit")
  expect_identical(as.vector(class(as_stanfit(native))), "stanli_stanfit")
})

test_that("a plain stanfit can have its model attached again", {
  m <- stanmodel_fixture()
  model <- stanli_model(code = normal_code, data = normal_data())
  fit <- rstan::sampling(m, data = normal_data(), chains = 1, iter = 60, warmup = 30,
                         seed = 1, refresh = 0)
  restored <- as_stanfit(as_stanfit(fit, plain = TRUE), model = model)
  expect_s4_class(restored, "stanli_stanfit")
  expect_equal(rstan::log_prob(restored, c(0, 0)), rstan::log_prob(fit, c(0, 0)))
})
