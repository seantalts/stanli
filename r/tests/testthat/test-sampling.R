test_that("a model compiles and reports its shape", {
  skip_without_runtime()
  m <- es_model()
  expect_s3_class(m, "stanli_model")
  expect_equal(m$n_unconstrained, 10L)
  expect_true(all(c("mu", "tau", "theta[1]") %in% m$columns))
})

test_that("columns index with brackets the way posterior reads them", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { real s; vector[2] v; matrix[2, 3] M; }
    model { s ~ std_normal(); v ~ std_normal(); to_vector(M) ~ std_normal(); }")
  expect_equal(m$columns, c("s", "v[1]", "v[2]", "M[1,1]", "M[2,1]",
                            "M[1,2]", "M[2,2]", "M[1,3]", "M[2,3]"))
  fit <- sample_model(m, chains = 1, warmup = 20, samples = 5, refresh = 0)
  expect_equal(dimnames(fit$draws)[[3]], m$columns)
  skip_if_not_installed("posterior")
  rv <- posterior::as_draws_rvars(as_draws_array(fit))
  expect_equal(dim(rv$M), c(2L, 3L))
  expect_equal(length(rv$v), 2L)
})

test_that("log_prob_grad returns lp and a gradient of the right length", {
  skip_without_runtime()
  m <- es_model()
  g <- log_prob_grad(m, rep(0, m$n_unconstrained))
  expect_true(is.finite(g$lp))
  expect_length(g$grad, m$n_unconstrained)
})

test_that("a run seed rebuilds transformed data the way CmdStan seeds it", {
  skip_without_runtime()
  code <- "
    transformed data { real z = normal_rng(0, 1); }
    parameters { real mu; }
    model { mu ~ normal(z, 1); }"
  lp_at <- function(m) log_prob_grad(m, 0)$lp
  seven <- lp_at(stanli_model(code = code, seed = 7))
  expect_equal(lp_at(stanli_model(code = code, seed = 7)), seven)
  m <- stanli_model(code = code, seed = 8)
  expect_false(lp_at(m) == seven)
  fit <- sample_model(m, chains = 1, seed = 7, warmup = 10, samples = 10,
                      refresh = 0)
  expect_equal(fit$model$seed, 7)
  expect_identical(fit$model$model_code, code)
  expect_identical(fit$model$model_name, m$model_name)
  expect_equal(lp_at(fit$model), seven)
  # A model whose transformed data never draws keeps its handle.
  plain <- progress_model()
  fit <- sample_model(plain, chains = 1, seed = 7, warmup = 10, samples = 10,
                      refresh = 0)
  expect_identical(fit$model$ptr, plain$ptr)
})

test_that("a run-seed rebuild refreshes the free vector and the columns", {
  skip_without_runtime()
  # Transformed data can size a parameter, so the rebuild can change the
  # free vector and the columns, not only the draws. The model the fit
  # carries must describe what it now holds; the caller's object is a value
  # and keeps its own seed.
  code <- "
    transformed data { int k = 1 + poisson_rng(2.0); }
    parameters { vector[k] mu; }
    model { mu ~ normal(0, 1); }"
  first <- stanli_model(code = code, seed = 1)
  other <- 2
  while (stanli_model(code = code, seed = other)$n_unconstrained ==
         first$n_unconstrained) other <- other + 1
  fresh <- stanli_model(code = code, seed = other)
  fit <- sample_model(first, chains = 1, seed = other, warmup = 10,
                      samples = 10, refresh = 0)
  expect_equal(fit$model$n_unconstrained, fresh$n_unconstrained)
  expect_identical(fit$model$columns, fresh$columns)
  expect_identical(dimnames(fit$draws)[[3]], fresh$columns)
  q <- rep(0, fresh$n_unconstrained)
  expect_equal(log_prob_grad(fit$model, q)$lp, log_prob_grad(fresh, q)$lp)
  expect_equal(first$seed, 1)
})

test_that("unconstrain turns constrained starting values into the free vector", {
  skip_without_runtime()
  # A model whose free order differs from its serial order: the simplex has
  # one fewer free value than constrained, and the array batches sit
  # contiguously in the free vector while the CSV lists the first index
  # fastest.
  m <- stanli_model(code = "
    parameters { real mu; real<lower=0> sigma; array[2] simplex[3] s; }
    model {
      mu ~ normal(0, 1); sigma ~ normal(0, 1);
      for (i in 1:2) s[i] ~ dirichlet(rep_vector(1.0, 3));
    }")
  q <- unconstrain(m, list(
    mu = 0.5, sigma = 1.25,
    s = matrix(c(0.2, 0.3, 0.5, 0.1, 0.6, 0.3), nrow = 2, byrow = TRUE)))
  expect_length(q, m$n_unconstrained)
  expect_true(all(is.finite(q)))
  # sigma is log(1.25) because its only transform is the lower bound.
  expect_equal(q[2], log(1.25))
  expect_equal(q[1], 0.5)
})

test_that("unconstrain names a parameter it cannot use", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { real mu; real<lower=0> sigma; }
    model { mu ~ normal(0, 1); sigma ~ normal(0, 1); }")
  expect_error(unconstrain(m, list(mu = 0)), "sigma")
  expect_error(unconstrain(m, list(mu = 0, sigma = -1)), "sigma")
  expect_error(
    unconstrain(m, list(mu = 0, sigma = 1, not_a_parameter = 2)),
    "not_a_parameter")
})

test_that("refresh is a single nonnegative integer", {
  bad <- list(-1, 1.5, NA_real_, NaN, Inf, c(1, 2), "1", TRUE, NULL)
  for (value in bad)
    expect_error(sample_model(NULL, refresh = value),
                 "single nonnegative integer", fixed = TRUE)
})

test_that("sampling progress is informative and observational", {
  skip_without_runtime()
  m <- progress_model()
  loud <- NULL
  text <- capture.output({
    loud <- sample_model(m, chains = 1, seed = 9182, warmup = 3, samples = 4,
                         init = 0, init_radius = 0, parallel_chains = 1,
                         refresh = 2)
  })

  progress <- grep("^Chain \\[1\\] Iteration:", text, value = TRUE)
  iterations <- as.integer(sub("^.*Iteration: +([0-9]+) /.*$", "\\1",
                               progress))
  expect_identical(iterations, c(1L, 2L, 3L, 4L, 5L, 7L))
  expect_true(all(grepl("\\(Warmup\\)$", progress[1:3])))
  expect_true(all(grepl("\\(Sampling\\)$", progress[4:6])))
  expect_equal(sum(grepl("^Chain \\[1\\] Elapsed Time:", text)), 1)
  expect_true(loud$report$available)
  expect_length(loud$report$warmup_seconds, 1)
  expect_gte(loud$report$warmup_seconds, 0)
  expect_gte(loud$report$sampling_seconds, 0)
  expect_gte(loud$report$n_divergent, 0)
  expect_gte(loud$report$n_max_treedepth, 0)
  expect_identical(names(loud)[seq_len(7)],
                   c("draws", "sampler", "unconstrained", "columns",
                     "max_depth", "seed", "model"))

  quiet <- NULL
  quiet_text <- capture.output({
    quiet <- sample_model(m, chains = 1, seed = 9182, warmup = 3, samples = 4,
                          init = 0, init_radius = 0, parallel_chains = 1,
                          refresh = 0)
  })
  expect_length(quiet_text, 0)
  expect_identical(loud$draws, quiet$draws)
  expect_identical(loud$sampler, quiet$sampler)
  expect_identical(loud$unconstrained, quiet$unconstrained)
  expect_identical(loud$report$n_divergent, quiet$report$n_divergent)
  expect_identical(loud$report$n_max_treedepth,
                   quiet$report$n_max_treedepth)
})

test_that("problem output uses exact unthinned report counts", {
  skip_without_runtime()
  m <- progress_model()
  fit <- NULL
  text <- capture.output({
    fit <- sample_model(m, chains = 1, seed = 7, warmup = 10, samples = 20,
                        thin = 3, max_depth = 1, init = 0, init_radius = 0,
                        parallel_chains = 1, refresh = 100)
  })

  expect_gt(fit$report$n_max_treedepth, dim(fit$sampler)[1])
  expect_true(any(grepl(
    paste0("Warning: ", fit$report$n_max_treedepth,
           " of 20 post-warmup transitions saturated"),
    text, fixed = TRUE)))
})

test_that("parallel chains report through the R console", {
  skip_without_runtime()
  m <- progress_model()
  text <- capture.output(
    sample_model(m, chains = 2, seed = 7, warmup = 2, samples = 2,
                 init = 0, init_radius = 0, parallel_chains = 2, refresh = 1))

  for (chain in 1:2) {
    progress <- grep(paste0("^Chain \\[", chain, "\\] Iteration:"), text,
                     value = TRUE)
    iterations <- as.integer(sub("^.*Iteration: +([0-9]+) /.*$", "\\1",
                                 progress))
    expect_identical(iterations, 1:4)
    expect_equal(sum(grepl(paste0("^Chain \\[", chain,
                                  "\\] Elapsed Time:"), text)), 1)
  }
})

test_that("sampling recovers the eight schools posterior", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 4, seed = 1, warmup = 1000,
                      samples = 1000, refresh = 0)
  expect_equal(dim(fit$draws)[1:2], c(1000L, 4L))
  s <- summary(fit)
  mu <- s[s$variable == "mu", ]
  expect_gt(mu$mean, 3)
  expect_lt(mu$mean, 6)
  expect_lt(max(s$rhat, na.rm = TRUE), 1.05)
  expect_gt(min(s$ess_bulk, na.rm = TRUE), 100)
})

test_that("threading does not change the answer", {
  skip_without_runtime()
  m <- es_model()
  a <- sample_model(m, chains = 4, seed = 7, warmup = 300, samples = 300,
                    parallel_chains = 1, refresh = 0)
  b <- sample_model(m, chains = 4, seed = 7, warmup = 300, samples = 300,
                    parallel_chains = 4, refresh = 0)
  # Each chain owns its executor and its RNG stream, so a parallel run is
  # byte-identical. This holds on a single-threaded build too, which is
  # the point: turning threads on cannot quietly change a result.
  expect_identical(a$draws, b$draws)
})

test_that("chains are different streams of the same seed", {
  skip_without_runtime()
  m <- es_model()
  a <- sample_model(m, chains = 2, seed = 3, warmup = 200, samples = 200,
                    refresh = 0)
  b <- sample_model(m, chains = 2, seed = 3, warmup = 200, samples = 200,
                    refresh = 0)
  expect_identical(a$draws, b$draws)
  # Identical chains would mean the chain id never reached the RNG, and
  # R-hat of two identical chains is a clean 1.0 -- nothing would show it.
  expect_false(identical(a$draws[, 1, "mu"], a$draws[, 2, "mu"]))
})

test_that("Pathfinder initialization is reproducible across chains", {
  skip_without_runtime()
  m <- progress_model()
  options <- list(num_iterations = 100L, num_elbo_draws = 10L,
                  history_size = 5L, init_radius = 2)
  a <- sample_model(m, chains = 2, seed = 303, warmup = 30, samples = 20,
                    pathfinder_init = options, parallel_chains = 1,
                    refresh = 0)
  b <- sample_model(m, chains = 2, seed = 303, warmup = 30, samples = 20,
                    pathfinder_init = options, parallel_chains = 2,
                    refresh = 0)
  expect_identical(a$draws, b$draws)
  expect_identical(a$sampler, b$sampler)
  expect_false(identical(a$draws[, 1, "x"], a$draws[, 2, "x"]))
})

test_that("Pathfinder initialization validates its dedicated options", {
  expect_identical(
    stanli:::.pathfinder_init_options(list()),
    list(num_iterations = 1000L, num_elbo_draws = 25L,
         history_size = 5L, init_radius = 2))
  bad <- list(
    1,
    list(1),
    list(not_an_option = 1),
    list(num_iterations = 0),
    list(num_elbo_draws = 1.5),
    list(history_size = TRUE),
    list(init_radius = NaN),
    list(init_radius = -1))
  for (value in bad)
    expect_error(stanli:::.pathfinder_init_options(value), "pathfinder_init")
  expect_error(
    sample_model(NULL, init = 0, pathfinder_init = list(), refresh = 0),
    "mutually exclusive", fixed = TRUE)
})

test_that("the draws array is posterior-shaped and keeps its names", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 2, seed = 4, warmup = 200,
                      samples = 200, refresh = 0)
  a <- as_draws_array(fit)
  # The dims carry names (iteration, chain, variable), which is what
  # posterior expects, so compare the values rather than the vector.
  expect_equal(unname(dim(a)[1:2]), c(200L, 2L))
  expect_true("mu" %in% dimnames(a)[[3]])
  b <- as_draws_array(fit, include_sampler = TRUE)
  expect_true(all(c("lp__", "divergent__", "mu") %in% dimnames(b)[[3]]))
})

test_that("diagnostics report on a converged fit", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 4, seed = 5, warmup = 1000,
                      samples = 1000, refresh = 0)
  txt <- capture.output(stanli_diagnose(fit))
  expect_true(any(grepl("R-hat is below", txt)))
  expect_true(any(grepl("E-BFMI is above", txt)))
})

test_that("optimize finds a mode the sampler can start from", {
  skip_without_runtime()
  m <- es_model()
  o <- optimize_model(m, seed = 1)
  expect_length(o$unconstrained, m$n_unconstrained)
  # The reported lp must be the model's lp there, not the objective the
  # optimizer minimizes.
  expect_equal(o$lp, log_prob_grad(m, o$unconstrained)$lp, tolerance = 1e-8)
  fit <- sample_model(m, chains = 1, seed = 2, warmup = 200, samples = 200,
                      init = o$unconstrained, refresh = 0)
  expect_equal(dim(fit$draws)[1], 200L)
})

test_that("data reaches the model in the right shape", {
  skip_without_runtime()
  # A matrix goes out row-major, which is what Stan's JSON reader wants;
  # transposing it silently would still compile and still sample.
  m <- stanli_model(code = "
    data { int N; int K; matrix[N, K] X; vector[N] y; }
    parameters { vector[K] b; real<lower=0> s; }
    model { y ~ normal(X * b, s); }",
    data = list(N = 3L, K = 2L,
                X = matrix(c(1, 2, 3, 4, 5, 6), nrow = 3, byrow = TRUE),
                y = c(1, 2, 3)))
  expect_equal(m$n_unconstrained, 3L)
  g <- log_prob_grad(m, c(0.1, 0.2, 0.3))
  expect_true(is.finite(g$lp))
})

test_that("a part with no compiled path warns, or errors when refused", {
  skip_without_runtime()
  code <- "
    parameters { real mu; }
    model { mu ~ normal(0, 1); }
    generated quantities {
      real s = 0;
      if (mu > 0) {
        matrix[1100, 1000] big = rep_matrix(mu, 1100, 1000);
        s = big[1, 1];
      }
    }"
  expect_warning(stanli_model(code = code), "interpreter")
  expect_warning(es_model(), NA)
  Sys.setenv(STANLI_NO_INTERPRETER = "1")
  on.exit(Sys.unsetenv("STANLI_NO_INTERPRETER"), add = TRUE)
  expect_error(stanli_model(code = code), "STANLI_NO_INTERPRETER")
})

test_that("rejected generated quantities preserve the chain", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { real mu; }
    model { mu ~ normal(0, 1); }
    generated quantities { int k = categorical_rng([mu, 1 - mu]'); }")
  fit <- sample_model(m, chains = 1, warmup = 20, samples = 20, refresh = 0)
  expect_equal(dim(fit$draws)[1], 20L)
  expect_equal(dim(fit$sampler), c(20L, 1L, 7L))
  expect_true(all(is.finite(fit$sampler)))
  expect_true(any(is.nan(fit$draws)))
})
