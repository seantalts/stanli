sampler_model <- function(shift = 0) {
  stanli_model(code = "
    data { int<lower=0> N; vector[N] y; }
    parameters { real mu; real<lower=0> sigma; vector[2] z; }
    model { y ~ normal(mu, sigma); z ~ std_normal(); sigma ~ exponential(1); }",
    data = list(N = 5L, y = c(-1, 0, 0.5, 1, 2) + shift))
}

sampler_fit <- function(...) {
  sample_model(sampler_model(), chains = 2, warmup = 100, samples = 50,
               refresh = 0, seed = 3, ...)
}

adaptation_lines <- function(text) strsplit(text, "\n", fixed = TRUE)[[1L]]

test_that("a dense metric is recorded as rstan records it", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- sampler_fit(metric = "dense_e")
  sf <- as_stanfit(fit)
  for (chain in 1:2) {
    args <- attr(sf@sim$samples[[chain]], "args")
    expect_identical(args$sampler_t, "NUTS(dense_e)")
    expect_identical(args$control$metric, "dense_e")
    expect_true(args$control$adapt_engaged)
    expect_identical(sf@stan_args[[chain]]$sampler_t, "NUTS(dense_e)")
    expect_identical(sf@stan_args[[chain]]$control$metric, "dense_e")
    expect_true(sf@stan_args[[chain]]$control$adapt_engaged)
  }
  info <- adaptation_lines(rstan::get_adaptation_info(sf)[[1L]])
  expect_identical(info[1:2], c("# Adaptation terminated",
                                paste("# Step size =",
                                      formatC(fit$step_size[1L], digits = 6, format = "g"))))
  expect_identical(info[3L], "# Elements of inverse mass matrix:")
  expect_length(info, 3L + 4L)
  rows <- do.call(rbind, lapply(strsplit(sub("^# ", "", info[4:7]), ", "),
                                as.numeric))
  expect_equal(rows, fit$inv_metric[[1L]], tolerance = 1e-5)
})

test_that("a diagonal metric and step size reach get_adaptation_info", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- sampler_fit()
  sf <- as_stanfit(fit)
  expect_identical(attr(sf@sim$samples[[1L]], "args")$sampler_t, "NUTS(diag_e)")
  info <- rstan::get_adaptation_info(sf)
  expect_length(info, 2L)
  for (chain in 1:2) {
    lines <- adaptation_lines(info[[chain]])
    expect_length(lines, 4L)
    expect_identical(lines[1L], "# Adaptation terminated")
    expect_equal(as.numeric(sub("^# Step size = ", "", lines[2L])),
                 fit$step_size[chain], tolerance = 1e-5)
    expect_identical(lines[3L], "# Diagonal elements of inverse mass matrix:")
    expect_equal(as.numeric(strsplit(sub("^# ", "", lines[4L]), ", ")[[1L]]),
                 fit$inv_metric[[chain]], tolerance = 1e-5)
    expect_true(endsWith(info[[chain]], "\n"))
  }
})

test_that("a unit metric has no free parameters to report", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  sf <- as_stanfit(sampler_fit(metric = "unit_e"))
  expect_identical(attr(sf@sim$samples[[1L]], "args")$sampler_t, "NUTS(unit_e)")
  lines <- adaptation_lines(rstan::get_adaptation_info(sf)[[1L]])
  expect_identical(lines[3L], "# No free parameters for unit metric")
})

test_that("a fixed step size without adaptation is recorded and reported as given", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  sf <- as_stanfit(sampler_fit(step_size = 0.3, adapt_engaged = FALSE))
  for (chain in 1:2) {
    control <- attr(sf@sim$samples[[chain]], "args")$control
    expect_identical(control$stepsize, 0.3)
    expect_false(control$adapt_engaged)
    expect_identical(sf@stan_args[[chain]]$control$stepsize, 0.3)
    expect_false(sf@stan_args[[chain]]$control$adapt_engaged)
  }
  expect_null(rstan::get_adaptation_info(sf))
})

test_that("an initial step size is recorded when adaptation moves it", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- sampler_fit(step_size = 0.3)
  sf <- as_stanfit(fit)
  control <- attr(sf@sim$samples[[1L]], "args")$control
  expect_identical(control$stepsize, 0.3)
  expect_true(control$adapt_engaged)
  expect_false(isTRUE(all.equal(fit$step_size[1L], 0.3)))
  expect_equal(as.numeric(sub("^# Step size = ", "",
                              adaptation_lines(rstan::get_adaptation_info(sf)[[1L]])[2L])),
               fit$step_size[1L], tolerance = 1e-5)
})

test_that("the default sampler records Stan's starting step size", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  sf <- as_stanfit(sampler_fit())
  control <- attr(sf@sim$samples[[1L]], "args")$control
  expect_identical(control, list(adapt_delta = 0.8, max_treedepth = 10,
                                 metric = "diag_e", adapt_engaged = TRUE,
                                 stepsize = 1))
  expect_identical(sf@stan_args[[1L]]$control, control)
  expect_identical(sf@stan_args[[1L]]$sampler_t, "NUTS(diag_e)")
})

test_that("fits without the sampler fields still convert", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  fit <- sampler_fit()
  for (field in c("metric", "step_size", "inv_metric", "adapt_engaged",
                  "initial_step_size"))
    fit[[field]] <- NULL
  sf <- as_stanfit(fit)
  args <- attr(sf@sim$samples[[1L]], "args")
  expect_identical(args$sampler_t, "NUTS(diag_e)")
  expect_identical(args$control, list(adapt_delta = 0.8, max_treedepth = 10,
                                      metric = "diag_e", adapt_engaged = TRUE))
  expect_null(rstan::get_adaptation_info(sf))
})

test_that("fixed-parameter fits keep their settings", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  model <- stanli_model(code = "
    parameters { real mu; } model { mu ~ normal(0, 1); }
    generated quantities { real y = normal_rng(0, 1); }")
  sf <- as_stanfit(sample_model(model, chains = 1, warmup = 0, samples = 20,
                                refresh = 0, fixed_param = TRUE))
  args <- attr(sf@sim$samples[[1L]], "args")
  expect_identical(args$sampler_t, "Fixed_param")
  expect_null(args$control)
  expect_null(rstan::get_adaptation_info(sf))
})

test_that("a combined plain stanfit takes a model back", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  first_model <- sampler_model()
  second_model <- sampler_model(shift = 1)
  first <- sample_model(first_model, chains = 1, warmup = 100, samples = 50,
                        refresh = 0, seed = 1)
  second <- sample_model(second_model, chains = 1, warmup = 100, samples = 50,
                         refresh = 0, seed = 2)
  combined <- rstan::sflist2stanfit(list(as_stanfit(first), as_stanfit(second)))
  expect_false(is(combined, "stanli_stanfit"))
  expect_error(as_stanfit(combined), "model")
  expect_error(as_stanfit(combined, model = NULL), "model")

  sf <- as_stanfit(combined, model = first_model)
  expect_s4_class(sf, "stanli_stanfit")
  expect_identical(sf@sim$samples, combined@sim$samples)
  expect_identical(sf@sim$fnames_oi, combined@sim$fnames_oi)
  expect_identical(rstan::get_num_upars(sf), 4L)
  upars <- c(0.2, -0.4, 0.1, 0.3)
  reference <- as_stanfit(first)
  expect_equal(rstan::log_prob(sf, upars), rstan::log_prob(reference, upars))
  expect_equal(rstan::grad_log_prob(sf, upars),
               rstan::grad_log_prob(reference, upars))
  expect_equal(rstan::constrain_pars(sf, upars),
               rstan::constrain_pars(reference, upars))
  expect_equal(rstan::unconstrain_pars(sf, rstan::constrain_pars(sf, upars)),
               upars)

  other <- stanli_model(code = "parameters { real mu; } model { mu ~ std_normal(); }")
  expect_error(as_stanfit(combined, model = other), "model must match")
  expect_error(as_stanfit(combined, model = "model"), "model must match")

  renamed <- combined
  renamed@sim$fnames_oi[1L] <- "b_renamed"
  again <- as_stanfit(renamed, model = first_model)
  expect_identical(again@sim$fnames_oi[1L], "b_renamed")
  expect_equal(rstan::log_prob(again, upars), rstan::log_prob(reference, upars))
  expect_error(as_stanfit(renamed, model = other), "model must match")
})
