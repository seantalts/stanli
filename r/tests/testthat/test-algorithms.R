# Fixed-parameter sampling, Laplace sampling, Pathfinder and ADVI.
#
# The constant in the model block is there for ADVI, which stops on the
# RELATIVE change of its lower bound. A normalized density has a lower bound
# near zero at the optimum, so that ratio never settles; a real posterior's
# is far from zero, and the constant makes this one so too.

gaussian_model <- function() {
  stanli_model(code = "
    data { vector[2] m; cov_matrix[2] S; }
    parameters { vector[2] theta; real<lower=0> tau; }
    model {
      theta ~ multi_normal(m, S);
      tau ~ lognormal(0, 0.5);
      target += -100;
    }
    generated quantities {
      real tau2 = square(tau);
      real noise = normal_rng(theta[1], 1);
    }", data = list(m = c(1, -2), S = matrix(c(1, 0.8, 0.8, 1), 2)))
}

gaussian_columns <- c("theta[1]", "theta[2]", "tau", "tau2", "noise")

expect_approximation_fit <- function(fit, algorithm, draws, sampler) {
  expect_s3_class(fit, "stanli_fit")
  expect_identical(fit$algorithm, algorithm)
  expect_identical(dim(fit$draws), c(as.integer(draws), 1L, 5L))
  expect_identical(dimnames(fit$draws)[[3L]], gaussian_columns)
  expect_identical(dimnames(fit$sampler)[[3L]], sampler)
  expect_identical(c(fit$chains, fit$warmup, fit$samples),
                   c(1L, 0L, as.integer(draws)))
  expect_true(all(is.finite(fit$draws)))
  expect_true(all(is.finite(fit$sampler)))
  expect_true(fit$report$sampling_seconds >= 0)
}

test_that("laplace_model draws from the normal approximation at the mode", {
  skip_without_runtime()
  model <- gaussian_model()
  fit <- laplace_model(model, seed = 11, draws = 200)
  expect_approximation_fit(fit, "laplace", 200, c("lp__", "lp_approx__"))
  expect_equal(fit$mode, c(1, -2, 0), tolerance = 1e-4)

  expect_identical(laplace_model(model, seed = 11, draws = 200)$draws, fit$draws)
  expect_false(identical(laplace_model(model, seed = 12, draws = 200)$draws,
                         fit$draws))
  # a mode from optimize_model(), or a bare unconstrained point
  mode <- optimize_model(model, seed = 3)
  from_list <- laplace_model(model, seed = 11, draws = 50, mode = mode)
  from_point <- laplace_model(model, seed = 11, draws = 50,
                              mode = mode$unconstrained)
  expect_identical(from_list$draws, from_point$draws)
  expect_true(all(is.nan(
    laplace_model(model, draws = 5, calculate_lp = FALSE)$sampler[, 1L, "lp__"])))

  expect_error(laplace_model(model, jacobian = FALSE), "jacobian = FALSE")
  expect_error(laplace_model(model, mode = c(1, 2)), "mode must be")
  expect_error(laplace_model(model, draws = 0), "draws must be")
})

test_that("pathfinder_model draws from a single path", {
  skip_without_runtime()
  model <- gaussian_model()
  fit <- pathfinder_model(model, seed = 21, draws = 200)
  expect_approximation_fit(fit, "pathfinder", 200,
                           c("lp__", "lp_approx__", "path__"))
  expect_true(all(fit$sampler[, 1L, "path__"] == 1))
  expect_identical(pathfinder_model(model, seed = 21, draws = 200)$draws,
                   fit$draws)
  expect_identical(pathfinder_model(model, seed = 21, draws = 200,
                                    num_paths = 1)$draws, fit$draws)
  expect_false(identical(pathfinder_model(model, seed = 22, draws = 200)$draws,
                         fit$draws))

  # a starting point is used, and the run depends on it
  from_a <- pathfinder_model(model, seed = 21, draws = 100, init = c(0, 0, 1))
  from_b <- pathfinder_model(model, seed = 21, draws = 100, init = c(2, -1, -1))
  expect_identical(dim(from_a$draws)[1L], 100L)
  expect_false(identical(from_a$draws, from_b$draws))
  # Stan scores its first num_elbo_draws draws whatever is asked
  bare <- pathfinder_model(model, seed = 21, draws = 60, calculate_lp = FALSE)
  expect_identical(is.nan(bare$sampler[, 1L, "lp__"]), rep(c(FALSE, TRUE), c(25, 35)))

  expect_error(pathfinder_model(model, init = c(0, 0)), "init must be")
  expect_error(pathfinder_model(model, num_paths = 0), "num_paths must be")
  expect_error(pathfinder_model(model, draws = 0), "draws must be")
  # Stan's own failure, reported with its message: L-BFGS cannot leave the mode
  expect_error(pathfinder_model(model, init = c(1, -2, 0)), "pathfinder failed")
})

test_that("multi-path Pathfinder is refused", {
  skip_without_runtime()
  model <- gaussian_model()
  # Stan 2.40's multi-path service reads past the end of an array, so it is
  # never called: more than one path is an error that says so.
  for (paths in c(2, 4))
    expect_error(pathfinder_model(model, num_paths = paths),
                 "multi-path Pathfinder is not available.*num_paths = 1")
  # the runtime refuses it too, for callers that do not come through here
  opts <- list(1L, 2L, 100L, 100L, 25L, 1000L, 5L, 0.001, 1e-12, 1e4, 1e-8,
               1e7, 1e-8, 2, TRUE, TRUE, 0L)
  expect_error(.Call("stanli_r_pathfinder", model$ptr, opts, numeric(0)),
               "num_paths must be 1")
  expect_false("single_path_draws" %in% names(formals(pathfinder_model)))
})

test_that("variational_model fits both ADVI families", {
  skip_without_runtime()
  model <- gaussian_model()
  full <- variational_model(model, "fullrank", seed = 31, draws = 200,
                            tol_rel_obj = 0.001, grad_samples = 10, refresh = 0)
  expect_approximation_fit(full, "fullrank", 200, c("lp__", "lp_approx__"))
  expect_named(full$mean, gaussian_columns)

  mean_field <- variational_model(model, seed = 31, draws = 200,
                                  tol_rel_obj = 0.001, grad_samples = 10,
                                  refresh = 0)
  expect_approximation_fit(mean_field, "meanfield", 200,
                           c("lp__", "lp_approx__"))
  expect_false(identical(mean_field$draws, full$draws))

  again <- variational_model(model, seed = 31, draws = 200,
                             tol_rel_obj = 0.001, grad_samples = 10, refresh = 0)
  expect_identical(again$draws, mean_field$draws)

  # Stan's progress is printed on request, and running out of iterations is
  # a warning with the fit reached so far.
  expect_output(variational_model(model, draws = 10, refresh = 1), "ELBO")
  expect_silent(variational_model(model, draws = 10, refresh = 0))
  expect_warning(
    short <- variational_model(model, iter = 150, adapt_engaged = FALSE,
                               eta = 0.1, draws = 10, refresh = 0),
    "maximum number of iterations")
  expect_identical(dim(short$draws)[1L], 10L)
  expect_error(variational_model(model, "laplace"), "should be one of")
  expect_error(variational_model(model, iter = 0), "iter must be")
  expect_error(variational_model(model, eta = -1), "eta must be")
})

test_that("sample_model(fixed_param = TRUE) moves only generated quantities", {
  skip_without_runtime()
  model <- gaussian_model()
  start <- unconstrain(model, list(theta = c(0.3, -0.7), tau = 2))
  fit <- sample_model(model, fixed_param = TRUE, chains = 2, samples = 50,
                      seed = 41, init = start, refresh = 0)
  expect_identical(fit$algorithm, "fixed_param")
  expect_identical(dim(fit$draws), c(50L, 2L, 5L))
  expect_identical(dimnames(fit$sampler)[[3L]], c("lp__", "accept_stat__"))
  expect_true(all(fit$sampler == 0))
  expect_length(fit$report$sampling_seconds, 2L)
  expect_true(all(fit$report$sampling_seconds >= 0))
  expect_true(all(fit$draws[, , "theta[1]"] == 0.3))
  expect_true(all(fit$draws[, , "theta[2]"] == -0.7))
  expect_equal(unique(as.vector(fit$draws[, , "tau2"])), 4)
  expect_false(identical(fit$draws[, 1L, "noise"], fit$draws[, 2L, "noise"]))
  again <- sample_model(model, fixed_param = TRUE, chains = 2, samples = 50,
                        seed = 41, init = start, refresh = 0)
  expect_identical(again$draws, fit$draws)

  # random starts differ by chain and stay put; thinning keeps every third
  thinned <- sample_model(model, fixed_param = TRUE, chains = 2, samples = 10,
                          thin = 3, seed = 41, refresh = 0)
  expect_identical(dim(thinned$draws), c(4L, 2L, 5L))
  expect_length(unique(thinned$draws[, 1L, "tau"]), 1L)
  expect_false(thinned$draws[1L, 1L, "tau"] == thinned$draws[1L, 2L, "tau"])

  # a model with nothing to sample
  empty <- stanli_model(code = "
    generated quantities { real y = normal_rng(5, 2); int k = poisson_rng(3); }")
  drawn <- sample_model(empty, fixed_param = TRUE, chains = 1, samples = 20,
                        seed = 42, refresh = 0)
  expect_identical(dimnames(drawn$draws)[[3L]], c("y", "k"))
  expect_true(all(drawn$draws[, 1L, "k"] == round(drawn$draws[, 1L, "k"])))

  expect_error(sample_model(model, fixed_param = TRUE, pathfinder_init = list()),
               "pathfinder_init does not apply")
  expect_error(sample_model(model, fixed_param = NA), "fixed_param must be")
})

test_that("fits from the other algorithms work as draws", {
  skip_without_runtime()
  model <- gaussian_model()
  fit <- pathfinder_model(model, seed = 51, draws = 200)
  arr <- as_draws_array(fit)
  expect_identical(unname(dim(arr)), c(200L, 1L, 5L))
  with_sampler <- as_draws_array(fit, include_sampler = TRUE)
  expect_identical(dimnames(with_sampler)[[3L]],
                   c("lp__", "lp_approx__", "path__", gaussian_columns))
  expect_identical(summary(fit)$variable, gaussian_columns)
  expect_output(print(fit), "1 chains x 200 draws")

  expect_error(stanli_diagnose(fit), "needs a NUTS fit.*'pathfinder'")
  expect_error(as_cstanfit(fit), "needs a NUTS fit")
  expect_error(get_sampler_params(fit), "needs a NUTS fit")
  skip_if_not_installed("posterior")
  expect_s3_class(arr, "draws_array")
  expect_identical(posterior::ndraws(posterior::as_draws_df(arr)), 200L)
  skip_if_not_installed("bayesplot")
  expect_error(bayesplot::nuts_params(fit), "needs a NUTS fit")
})

test_that("as_stanfit converts fits from the other algorithms", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  model <- gaussian_model()
  fits <- list(
    meanfield = variational_model(model, seed = 61, draws = 300, refresh = 0),
    fullrank = variational_model(model, "fullrank", seed = 61, draws = 300,
                                 refresh = 0),
    pathfinder = pathfinder_model(model, seed = 61, draws = 300),
    laplace = laplace_model(model, seed = 61, draws = 300))
  methods <- c(meanfield = "variational", fullrank = "variational",
               pathfinder = "pathfinder", laplace = "laplace")
  for (name in names(fits)) {
    fit <- fits[[name]]
    sf <- as_stanfit(fit)
    expect_s4_class(sf, "stanfit")
    expect_identical(sf@stan_args[[1L]]$method, methods[[name]], info = name)
    if (methods[[name]] == "variational")
      expect_identical(sf@stan_args[[1L]]$algorithm, name)
    # what brms's reader builds from CmdStan's output for these algorithms:
    # one chain, no warmup, the special variables last, no sampler columns
    expect_identical(sf@sim$fnames_oi,
                     c(gaussian_columns, "lp__", "lp_approx__"), info = name)
    expect_identical(sf@sim$pars_oi,
                     c("theta", "tau", "tau2", "noise", "lp__", "lp_approx__"))
    expect_identical(c(sf@sim$chains, sf@sim$warmup, sf@sim$iter),
                     c(1L, 0L, 300L))
    expect_identical(sf@sim$n_save, 300L)
    expect_identical(sf@sim$warmup2, 0L)
    expect_identical(ncol(attr(sf@sim$samples[[1L]], "sampler_params")), 0L)
    expect_identical(sf@sim$samples[[1L]]$lp__, fit$sampler[, 1L, "lp__"])
    expect_identical(sf@sim$samples[[1L]]$lp_approx__,
                     fit$sampler[, 1L, "lp_approx__"])
    expect_identical(sf@sim$samples[[1L]]$theta.1, fit$draws[, 1L, "theta[1]"])

    # rstan's own methods read it
    extracted <- rstan::extract(sf)
    expect_named(extracted, c("theta", "tau", "tau2", "noise", "lp__",
                              "lp_approx__"))
    expect_identical(dim(extracted$theta), c(300L, 2L))
    table <- rstan::summary(sf)$summary
    expect_identical(rownames(table), c(gaussian_columns, "lp__", "lp_approx__"))
    expect_equal(unname(table[gaussian_columns, "mean"]),
                 unname(colMeans(fit$draws[, 1L, ])))
    expect_identical(colnames(table)[ncol(table)],
                     if (methods[[name]] == "variational") "khat" else "Rhat")
    expect_identical(dim(as.array(sf)), c(300L, 1L, 7L))
    expect_equal(unname(rstan::get_elapsed_time(sf)[1L, "warmup"]), 0)
    # the live model still answers
    expect_equal(rstan::log_prob(sf, c(0, 0, 0)),
                 log_prob_grad(model, c(0, 0, 0))$lp)
    excluded <- as_stanfit(fit, exclude = c("tau2", "noise"))
    expect_identical(excluded@sim$fnames_oi,
                     c("theta[1]", "theta[2]", "tau", "lp__", "lp_approx__"))
  }

  fixed <- sample_model(model, fixed_param = TRUE, chains = 2, samples = 100,
                        refresh = 0)
  sf <- as_stanfit(fixed)
  expect_identical(sf@stan_args[[1L]]$method, "sampling")
  expect_identical(sf@stan_args[[1L]]$algorithm, "Fixed_param")
  expect_identical(sf@sim$fnames_oi, c(gaussian_columns, "lp__"))
  expect_identical(names(attr(sf@sim$samples[[1L]], "sampler_params")),
                   "accept_stat__")
  expect_identical(dim(as.array(sf)), c(100L, 2L, 6L))
  expect_identical(dim(rstan::extract(sf)$noise), 200L)
  expect_identical(nrow(rstan::summary(sf)$summary), 6L)
})
