fast_math_code <- "
  data { int<lower=0> N; vector[N] x; vector[N] y; }
  parameters { real a; real b; real<lower=0> sigma; }
  model { y ~ normal(a + b * x, sigma); }"
fast_math_data <- list(N = 4L, x = c(1, 2, 3, 4), y = c(1.1, 1.9, 3.2, 3.9))

test_that("fast_math is off by default and opt-in per model", {
  skip_without_runtime()
  q <- c(0.3, 0.7, -0.2)
  default <- stanli_model(code = fast_math_code, data = fast_math_data)
  expect_false(default$fast_math)
  fast <- stanli_model(code = fast_math_code, data = fast_math_data,
                       fast_math = TRUE)
  expect_true(fast$fast_math)
  expect_equal(.Call("stanli_r_n_unconstrained", fast$ptr), 3L)

  rebuilt <- stanli:::with_run_seed(fast, seed = 7, threads_per_chain = 2)
  expect_true(rebuilt$fast_math)

  for (bad in list(1, "yes", NA, c(TRUE, FALSE), NULL))
    expect_error(stanli_model(code = fast_math_code, data = fast_math_data,
                              fast_math = bad), "fast_math")
})

test_that("a fast model samples", {
  skip_without_runtime()
  fast <- stanli_model(code = fast_math_code, data = fast_math_data,
                       fast_math = TRUE)
  fit <- sample_model(fast, chains = 1, warmup = 50, samples = 50,
                      seed = 3, refresh = 0)
  expect_true(fit$model$fast_math)
})
