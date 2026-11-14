resume_code <- "
  data { int<lower=1> N; vector[N] y; }
  parameters { real mu; real<lower=0> sigma; }
  model {
    mu ~ normal(0, 5);
    sigma ~ lognormal(0, 1);
    y ~ normal(mu, sigma);
  }
  generated quantities {
    real y_rep = normal_rng(mu, sigma);
    int k = poisson_rng(3);
  }"

resume_model <- function(y = c(0.3, -1.2, 2.1, 0.8, 1.5)) {
  stanli_model(code = resume_code, data = list(N = 5L, y = y))
}

test_that("sample_more continues a run exactly", {
  skip_without_runtime()
  m <- resume_model()
  threads <- if (stanli_thread_safe()) c(1L, 2L) else 1L
  for (metric in c("diag_e", "dense_e")) for (thin in c(1L, 3L))
    for (parallel in threads) for (save_warmup in c(FALSE, thin == 3L)) {
      run <- function(samples)
        sample_model(m, chains = 2, seed = 4, warmup = 150, samples = samples,
                     thin = thin, metric = metric, save_warmup = save_warmup,
                     parallel_chains = parallel, refresh = 0)
      one <- run(41)
      first <- run(20)
      kept <- first
      more <- sample_more(first, samples = 21, refresh = 0,
                          parallel_chains = parallel)
      label <- paste(metric, thin, parallel, save_warmup)
      expect_identical(more$draws, one$draws, label = label)
      expect_identical(more$sampler, one$sampler, label = label)
      expect_identical(more$unconstrained, one$unconstrained, label = label)
      expect_identical(more$resume, one$resume, label = label)
      expect_identical(more$report$n_divergent, one$report$n_divergent)
      expect_identical(more$report$n_max_treedepth,
                       one$report$n_max_treedepth)
      expect_identical(more$samples, 41L)
      expect_identical(more$step_size, first$step_size)
      expect_identical(more$inv_metric, first$inv_metric)
      expect_identical(more$report$warmup_seconds,
                       first$report$warmup_seconds)
      expect_true(all(more$report$sampling_seconds >=
                        first$report$sampling_seconds))
      expect_identical(first, kept)
      expect_identical(as_cstanfit(more)$draws(), as_cstanfit(one)$draws())
    }
})

test_that("sample_more numbers its progress from where the run stopped", {
  skip_without_runtime()
  first <- sample_model(resume_model(), chains = 1, seed = 3, warmup = 50,
                        samples = 20, refresh = 0)
  out <- capture.output(sample_more(first, samples = 20, refresh = 10))
  lines <- grep("Iteration:", out, value = TRUE)
  expect_match(lines[1], "Iteration:   21 / 40", fixed = TRUE)
  expect_match(lines[length(lines)], "Iteration:   40 / 40", fixed = TRUE)
})

test_that("sample_more refuses what it cannot continue", {
  skip_without_runtime()
  m <- resume_model()
  first <- sample_model(m, chains = 2, seed = 2, warmup = 100, samples = 10,
                        refresh = 0)
  expect_error(sample_more(first, seed = 2), "seed")
  expect_error(sample_more(first, thin = 2, warmup = 10), "thin, warmup")
  expect_error(sample_more(first, inv_metric = 1), "inv_metric")
  old <- first
  old$resume <- NULL
  expect_error(sample_more(old), "no resume record")
  expect_error(sample_more(list()), "stanli_fit")
  other <- resume_model(c(0.3, -1.2, 2.1, 0.8, 9.5))
  moved <- first
  moved$model <- other
  expect_error(sample_more(moved, samples = 5, refresh = 0),
               "different model or data")
  fixed <- sample_model(m, fixed_param = TRUE, chains = 1, samples = 5,
                        init = c(0, 0), refresh = 0)
  expect_error(sample_more(fixed), "needs a NUTS fit")
})
