test_that("bridge sampling recovers a normal model's marginal likelihood", {
  skip_without_runtime()
  skip_if_not_installed("bridgesampling")
  y <- c(0.3, -1.2, 0.8, 1.5, 0.1)
  model <- stanli_model(code = "
    data { int N; vector[N] y; }
    parameters { real mu; }
    model {
      target += normal_lpdf(mu | 0, 1);
      target += normal_lpdf(y | mu, 1);
    }", data = list(N = length(y), y = y))
  fit <- sample_model(model, seed = 1, refresh = 0)
  S <- diag(length(y)) + 1
  exact <- -0.5 * (length(y) * log(2 * pi) +
                   as.numeric(determinant(S)$modulus) +
                   sum(y * solve(S, y)))
  set.seed(1)
  bridge <- bridgesampling::bridge_sampler(fit, silent = TRUE)
  expect_s3_class(bridge, "bridge")
  expect_equal(bridge$logml, exact, tolerance = 0.05, scale = 1)
})

test_that("bridge sampling includes the Jacobian of constrained parameters", {
  skip_without_runtime()
  skip_if_not_installed("bridgesampling")
  y <- c(0L, 1L, 0L, 0L, 0L, 0L, 0L, 0L, 0L, 1L)
  model <- stanli_model(code = "
    data { int N; array[N] int<lower=0, upper=1> y; }
    parameters { real<lower=0, upper=1> theta; }
    model {
      target += beta_lpdf(theta | 1, 1);
      target += bernoulli_lpmf(y | theta);
    }", data = list(N = length(y), y = y))
  fit <- sample_model(model, seed = 1, refresh = 0)
  set.seed(1)
  bridge <- bridgesampling::bridge_sampler(fit, silent = TRUE)
  expect_equal(bridge$logml, lbeta(3, 9), tolerance = 0.05, scale = 1)
})

test_that("bridge sampling needs the fit's live model", {
  skip_without_runtime()
  skip_if_not_installed("bridgesampling")
  model <- stanli_model(code = "
    parameters { real mu; }
    model { target += normal_lpdf(mu | 0, 1); }")
  fit <- sample_model(model, chains = 1, warmup = 50, samples = 50, refresh = 0)
  file <- tempfile(fileext = ".rds")
  on.exit(unlink(file))
  saveRDS(fit, file)
  restored <- readRDS(file)
  expect_error(bridgesampling::bridge_sampler(restored, silent = TRUE),
               "no live stanli model")
})
