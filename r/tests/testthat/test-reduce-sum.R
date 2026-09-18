reduce_source <- "
functions {
  real partial(array[] real y, int first, int last, real mu) {
    return normal_lpdf(y | mu, 1);
  }
}
data { int N; array[N] real y; }
parameters { real mu; }
model { mu ~ normal(0, 1); target += reduce_sum(partial, y, 1000, mu); }
generated quantities { real draw = normal_rng(mu, 1); }
"

test_that("native within-chain reductions reach sampling and gradients", {
  skip_without_runtime()
  data <- list(N=10000L, y=seq(-1, 2, length.out=10000))
  m <- stanli_model(code=reduce_source, data=data)
  expect_equal(m$reduce_sum_count, 0L)
  args <- list(chains=2, warmup=25, samples=10, refresh=0, seed=123, max_depth=6)
  for (bad in list(0, -1, 1.5, TRUE, "2", NA_real_, Inf, 2^40))
    expect_error(sample_model(m, threads_per_chain=bad), "threads_per_chain")
  if (!stanli_thread_safe()) {
    expect_error(sample_model(m, threads_per_chain=2), "thread-safe")
    return(invisible(NULL))
  }
  a <- do.call(sample_model, c(list(m), args, list(threads_per_chain=4, parallel_chains=2)))
  expect_equal(a$model$reduce_sum_count, 1L)
  expect_length(a$model$reduce_sum_fallbacks, 0L)
  expect_equal(a$model$threads_per_chain, 4)
  expect_equal(log_prob_grad(a$model, .2), log_prob_grad(m, .2), tolerance=2e-12)
  expect_equal(log_prob_grad(a$model, .2)$grad, sum(data$y) - 10001 * .2,
               tolerance=2e-12)
  b <- do.call(sample_model, c(list(a$model), args, list(threads_per_chain=4, parallel_chains=1)))
  expect_identical(a$draws, b$draws)
  expect_identical(a$sampler, b$sampler)
  serial <- do.call(sample_model, c(list(m), args))
  restored <- do.call(sample_model, c(list(a$model), args))
  expect_equal(restored$model$reduce_sum_count, 0L)
  expect_identical(serial$draws, restored$draws)
  # R models remain values: preparing a threaded run does not mutate its input.
  expect_equal(m$threads_per_chain, 1)
  small <- stanli_model(code=reduce_source, data=list(N=3L, y=c(0,1,2)), threads_per_chain=4)
  expect_equal(small$reduce_sum_count, 0L)
  expect_gt(length(small$reduce_sum_fallbacks), 0)
  wrapper <- cstan_model(reduce_source)$sample(data=data, chains=1,
      iter_warmup=10, iter_sampling=5, threads_per_chain=2, refresh=0)
  expect_equal(wrapper$stanli_fit()$model$reduce_sum_count, 1L)
})

test_that("thread changes preserve the transformed data construction seed", {
  skip_without_runtime()
  skip_if_not(stanli_thread_safe())
  code <- sub("parameters ", "transformed data { real shift = normal_rng(0,1); } parameters ",
              reduce_source, fixed=TRUE)
  code <- sub("mu ~ normal(0, 1)", "mu ~ normal(shift, 1)", code, fixed=TRUE)
  data <- list(N=10000L, y=rep(.5, 10000))
  m <- stanli_model(code=code, data=data, seed=2)
  fresh <- stanli_model(code=code, data=data, seed=17, threads_per_chain=2)
  args <- list(chains=1, warmup=10, samples=5, seed=17, refresh=0,
               max_depth=5, threads_per_chain=2)
  a <- do.call(sample_model, c(list(m), args))
  b <- do.call(sample_model, c(list(fresh), args))
  expect_equal(a$model$seed, 17)
  expect_equal(a$model$reduce_sum_count, 1L)
  expect_identical(a$draws, b$draws)
})
