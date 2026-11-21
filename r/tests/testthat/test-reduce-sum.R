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
  data <- list(N=100000L, y=seq(-1, 2, length.out=100000))
  m <- stanli_model(code=reduce_source, data=data)
  expect_equal(m$reduce_sum_count, 0L)
  args <- list(chains=2, warmup=25, samples=10, refresh=0, seed=123, max_depth=6)
  for (bad in list(0, -1, 1.5, TRUE, "2", NA_real_, Inf, 2^40)) {
    expect_error(stanli_model(code=reduce_source, data=data, threads_per_chain=bad),
                 "threads_per_chain")
    expect_error(sample_model(m, threads_per_chain=bad), "threads_per_chain")
  }
  if (!stanli_thread_safe()) {
    expect_error(stanli_model(code=reduce_source, data=data, threads_per_chain=2),
                 "thread-safe")
    return(invisible(NULL))
  }
  threaded <- stanli_model(code=reduce_source, data=data, threads_per_chain=4)
  a <- do.call(sample_model, c(list(threaded), args, list(parallel_chains=2)))
  expect_equal(a$model$reduce_sum_count, 1L)
  expect_length(a$model$reduce_sum_fallbacks, 0L)
  expect_equal(a$model$threads_per_chain, 4)
  expect_identical(dim(a$draws)[1:2], c(10L, 2L))
  serial <- do.call(sample_model, c(list(m), args))
  expect_warning(restored <- do.call(sample_model, c(list(a$model), args,
                                                     list(threads_per_chain=1))),
                 class="deprecatedWarning")
  expect_equal(restored$model$reduce_sum_count, 0L)
  expect_identical(serial$draws, restored$draws)
  # R models remain values: the deprecated rebuild does not mutate its input.
  expect_equal(a$model$threads_per_chain, 4)
  small <- stanli_model(code=reduce_source, data=list(N=3L, y=c(0,1,2)), threads_per_chain=4)
  expect_equal(small$reduce_sum_count, 0L)
  expect_gt(length(small$reduce_sum_fallbacks), 0)
  wrapper <- expect_silent(cstan_model(reduce_source)$sample(data=data, chains=1,
      iter_warmup=150, iter_sampling=5, threads_per_chain=2, refresh=0))
  expect_equal(wrapper$stanli_fit()$model$reduce_sum_count, 1L)
  expect_equal(wrapper$stanli_fit()$model$threads_per_chain, 2)
})

test_that("sampling uses the thread count the model was built with", {
  skip_without_runtime()
  skip_without_threads()
  data <- list(N=100000L, y=seq(-1, 2, length.out=100000))
  m <- stanli_model(code=reduce_source, data=data, threads_per_chain=2)
  args <- list(chains=1, warmup=150, samples=5, refresh=0, max_depth=5)
  kept <- expect_silent(do.call(sample_model, c(list(m), args)))
  expect_equal(kept$model$threads_per_chain, 2)
  expect_true(identical(kept$model$ptr, m$ptr))
  same <- expect_silent(do.call(sample_model, c(list(m), args,
                                                list(threads_per_chain=2))))
  expect_true(identical(same$model$ptr, m$ptr))
  expect_warning(serial <- do.call(sample_model, c(list(m), args,
                                                   list(threads_per_chain=1))),
                 "stanli_model\\(threads_per_chain", class="deprecatedWarning")
  expect_equal(serial$model$threads_per_chain, 1)
  expect_equal(serial$model$reduce_sum_count, 0L)
  more <- expect_silent(sample_more(kept, samples=5, refresh=0))
  expect_true(identical(more$model$ptr, m$ptr))
  expect_silent(sample_more(kept, samples=5, refresh=0, threads_per_chain=2))
  expect_warning(more1 <- sample_more(kept, samples=5, refresh=0, threads_per_chain=1),
                 class="deprecatedWarning")
  expect_equal(more1$model$threads_per_chain, 1)
  path <- pathfinder_model(m, draws=5)
  expect_true(identical(path$model$ptr, m$ptr))
})

test_that("a seed rebuild keeps the model's threads and transformed data seed", {
  skip_without_runtime()
  skip_if_not(stanli_thread_safe())
  code <- sub("parameters ", "transformed data { real shift = normal_rng(0,1); } parameters ",
              reduce_source, fixed=TRUE)
  code <- sub("mu ~ normal(0, 1)", "mu ~ normal(shift, 1)", code, fixed=TRUE)
  data <- list(N=100000L, y=rep(.5, 100000))
  m <- stanli_model(code=code, data=data, seed=2, threads_per_chain=2)
  fresh <- stanli_model(code=code, data=data, seed=17, threads_per_chain=2)
  args <- list(chains=1, warmup=10, samples=5, seed=17, refresh=0, max_depth=5)
  a <- do.call(sample_model, c(list(m), args))
  b <- do.call(sample_model, c(list(fresh), args))
  expect_equal(a$model$seed, 17)
  expect_equal(a$model$threads_per_chain, 2)
  expect_equal(a$model$reduce_sum_count, 1L)
  expect_identical(a$draws, b$draws)
})
