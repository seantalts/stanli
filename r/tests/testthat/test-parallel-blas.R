parallel_blas_code <- "
  data { int<lower=0> N; vector[N] x; vector[N] y; }
  parameters { real a; real b; real<lower=0> sigma; }
  model { y ~ normal(a + b * x, sigma); }"
parallel_blas_data <- list(N = 4L, x = c(1, 2, 3, 4),
                           y = c(1.1, 1.9, 3.2, 3.9))

test_that("parallel_blas is off by default and opt-in per model", {
  skip_without_runtime()
  default <- stanli_model(code = parallel_blas_code, data = parallel_blas_data)
  expect_false(default$parallel_blas)
  threaded <- stanli_model(code = parallel_blas_code,
                           data = parallel_blas_data, parallel_blas = TRUE)
  expect_true(threaded$parallel_blas)
  expect_equal(.Call("stanli_r_n_unconstrained", threaded$ptr), 3L)

  for (bad in list(1, "yes", NA, c(TRUE, FALSE), NULL))
    expect_error(stanli_model(code = parallel_blas_code,
                              data = parallel_blas_data,
                              parallel_blas = bad), "parallel_blas")
})

test_that("parallel_blas survives a rebuild for a new thread count", {
  skip_without_runtime()
  skip_without_threads()
  threaded <- stanli_model(code = parallel_blas_code,
                           data = parallel_blas_data, parallel_blas = TRUE)
  rebuilt <- stanli:::with_run_seed(threaded, seed = 7, threads_per_chain = 2)
  expect_true(rebuilt$parallel_blas)
})
