# These need a runtime. Point STANLI_RUNTIME at a build (and
# STANLI_STANC at a stanc3 if that build does not embed one) to run them;
# they skip otherwise, so R CMD check passes on a machine with neither.
skip_without_runtime <- function() {
  if (!stanli_available()) skip("no stanli runtime installed")
}

es_model <- function() {
  code <- "
    data { int<lower=0> J; array[J] real y; array[J] real<lower=0> sigma; }
    parameters { real mu; real<lower=0> tau; vector[J] theta_tilde; }
    transformed parameters { vector[J] theta = mu + tau * theta_tilde; }
    model {
      mu ~ normal(0, 5); tau ~ cauchy(0, 5);
      theta_tilde ~ std_normal(); y ~ normal(theta, sigma);
    }"
  stanli_model(code = code, data = list(
    J = 8L, y = c(28, 8, -3, 7, -1, 1, 18, 12),
    sigma = c(15, 10, 16, 11, 9, 11, 10, 18)))
}

progress_model <- function() {
  stanli_model(code = "
    parameters { real x; }
    model { x ~ normal(0, 1); }")
}

test_that("a model compiles and reports its shape", {
  skip_without_runtime()
  m <- es_model()
  expect_s3_class(m, "stanli_model")
  expect_equal(m$n_unconstrained, 10L)
  expect_true(all(c("mu", "tau", "theta.1") %in% m$columns))
})

test_that("log_prob_grad returns lp and a gradient of the right length", {
  skip_without_runtime()
  m <- es_model()
  g <- log_prob_grad(m, rep(0, m$n_unconstrained))
  expect_true(is.finite(g$lp))
  expect_length(g$grad, m$n_unconstrained)
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
