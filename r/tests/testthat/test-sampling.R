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

test_that("sampling recovers the eight schools posterior", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 4, seed = 1, warmup = 1000,
                      samples = 1000)
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
                    parallel_chains = 1)
  b <- sample_model(m, chains = 4, seed = 7, warmup = 300, samples = 300,
                    parallel_chains = 4)
  # Each chain owns its executor and its RNG stream, so a parallel run is
  # byte-identical. This holds on a single-threaded build too, which is
  # the point: turning threads on cannot quietly change a result.
  expect_identical(a$draws, b$draws)
})

test_that("chains are different streams of the same seed", {
  skip_without_runtime()
  m <- es_model()
  a <- sample_model(m, chains = 2, seed = 3, warmup = 200, samples = 200)
  b <- sample_model(m, chains = 2, seed = 3, warmup = 200, samples = 200)
  expect_identical(a$draws, b$draws)
  # Identical chains would mean the chain id never reached the RNG, and
  # R-hat of two identical chains is a clean 1.0 -- nothing would show it.
  expect_false(identical(a$draws[, 1, "mu"], a$draws[, 2, "mu"]))
})

test_that("the draws array is posterior-shaped and keeps its names", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 2, seed = 4, warmup = 200,
                      samples = 200)
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
                      samples = 1000)
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
                      init = o$unconstrained)
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
