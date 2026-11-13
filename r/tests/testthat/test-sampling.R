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

test_that("parameter_names lists declared parameters, not outputs", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { real mu; vector<lower=0>[2] sigma; matrix[2, 3] B; }
    transformed parameters { real tp = mu; }
    model { mu ~ normal(0, 1); sigma ~ normal(0, 1); to_vector(B) ~ normal(0, 1); }
    generated quantities { real g = mu; }")
  expect_identical(parameter_names(m), c("mu", "sigma", "B"))
  scalars <- parameter_names(m, scalars = TRUE)
  expect_length(scalars, m$n_unconstrained)
  expect_identical(scalars[1:3], c("mu", "sigma[1]", "sigma[2]"))
  expect_true("B[2,3]" %in% scalars)
  expect_false(any(c("tp", "g") %in% scalars))
  expect_error(parameter_names(m, scalars = NA), "scalars")
  expect_error(parameter_names(list()), "stanli_model")
  none <- stanli_model(code = "data { int N; } model { }", data = list(N = 1L))
  expect_identical(parameter_names(none), character(0))
})

test_that("unconstrain can drop names that are not parameters", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { real mu; real<lower=0> sigma; }
    transformed parameters { real tp = mu; }
    model { mu ~ normal(0, 1); sigma ~ normal(0, 1); }")
  values <- list(mu = 0.5, sigma = 2, tp = 9, extra = 1:3)
  expect_error(unconstrain(m, values), "extra")
  expect_equal(unconstrain(m, values, ignore_unknown = TRUE),
               c(0.5, log(2)))
  expect_equal(unconstrain(m, values, partial = TRUE, ignore_unknown = TRUE),
               c(0.5, log(2)))
  some <- unconstrain(m, list(sigma = 2, tp = 1), partial = TRUE,
                      ignore_unknown = TRUE, seed = 3)
  expect_equal(some, unconstrain(m, list(sigma = 2), partial = TRUE, seed = 3))
  expect_equal(unconstrain(m, list(other = 1), partial = TRUE,
                           ignore_unknown = TRUE, init_radius = 0),
               c(0, 0))
  expect_error(unconstrain(m, values, ignore_unknown = NA), "ignore_unknown")
  expect_error(unconstrain(m, '{"mu": 1, "sigma": 1}', ignore_unknown = TRUE),
               "list")
  q <- unconstrain(m, values, ignore_unknown = TRUE)
  fit <- sample_model(m, chains = 1, warmup = 50, samples = 20, seed = 1, init = q,
                      refresh = 0)
  expect_equal(dim(fit$draws)[1], 20)
})

test_that("refresh is a single nonnegative integer", {
  bad <- list(-1, 1.5, NA_real_, NaN, Inf, c(1, 2), "1", TRUE, NULL)
  for (value in bad)
    expect_error(sample_model(NULL, refresh = value),
                 "single nonnegative integer", fixed = TRUE)
})

test_that("sample_model rejects bad counts and options by name", {
  skip_without_runtime()
  m <- progress_model()
  bad <- list(
    list(list(chains = 0), "chains must be an integer >= 1"),
    list(list(parallel_chains = 0), "parallel_chains must be an integer >= 1"),
    list(list(seed = -1), "seed must be an integer >= 0"),
    list(list(seed = "a"), "seed must be an integer >= 0"),
    list(list(chains = "a"), "chains must be an integer"),
    list(list(samples = 2.5), "samples must be an integer"),
    list(list(samples = NA), "samples must be an integer"),
    list(list(delta = "a"), "delta must be a number"))
  quick <- list(m, warmup = 5, samples = 5, refresh = 0)
  for (case in bad)
    expect_error(
      do.call(sample_model, utils::modifyList(quick, case[[1]])),
      case[[2]], fixed = TRUE)
  expect_no_warning(try(sample_model(m, seed = "a", warmup = 5, samples = 5,
                                     refresh = 0), silent = TRUE))
  fit <- sample_model(m, chains = 1, warmup = 0, samples = 3, refresh = 0)
  expect_equal(dim(fit$draws)[1], 3)
})

test_that("fixed_param and optimize name the argument they reject", {
  skip_without_runtime()
  m <- progress_model()
  expect_error(sample_model(m, chains = 0, fixed_param = TRUE, refresh = 0),
               "chains must be an integer >= 1", fixed = TRUE)
  expect_error(optimize_model(m, iter = 0), "iter must be an integer >= 1",
               fixed = TRUE)
  expect_error(optimize_model(m, iter = "a"), "iter must be an integer",
               fixed = TRUE)
  expect_error(optimize_model(m, seed = "a"), "seed must be an integer >= 0",
               fixed = TRUE)
  expect_error(optimize_model(m, init_radius = -1),
               "init_radius must be a finite number >= 0", fixed = TRUE)
})

test_that("sampling progress shows running post-warmup problem counts", {
  skip_without_runtime()
  m <- progress_model()
  # A fixed step size of 5 is unstable for a standard normal, so the chains
  # diverge in warmup and sampling alike.
  args <- list(m, chains = 2, seed = 552, warmup = 20, samples = 40,
               parallel_chains = 2, adapt_engaged = FALSE, step_size = 5)
  fit <- NULL
  text <- capture.output(fit <- do.call(sample_model, c(args, refresh = 1)))
  quiet_text <- capture.output(
    quiet <- do.call(sample_model, c(args, refresh = 0)))
  expect_length(quiet_text, 0)
  expect_identical(fit$draws, quiet$draws)
  expect_identical(fit$sampler, quiet$sampler)
  progress <- grep("Iteration:", text, fixed = TRUE, value = TRUE)
  expect_true(all(grepl("\\(Warmup\\)$",
                        grep("(Warmup)", progress, fixed = TRUE,
                             value = TRUE))))
  for (chain in 1:2) {
    wanted <- fit$report$n_divergent[chain]
    expect_gt(wanted, 0)
    sampling <- grep(sprintf("^Chain \\[%d\\].*\\(Sampling\\)", chain),
                     progress, value = TRUE)
    expect_match(sampling[length(sampling)],
                 sprintf("\\(Sampling\\)  %d divergent$", wanted))
  }

  text <- capture.output(
    fit <- sample_model(m, chains = 1, seed = 553, warmup = 30, samples = 40,
                        max_depth = 1, refresh = 1))
  progress <- grep("Iteration:", text, fixed = TRUE, value = TRUE)
  hits <- fit$report$n_max_treedepth
  expect_gt(hits, 0)
  expect_match(progress[length(progress)],
               sprintf("\\(Sampling\\)  %d at max treedepth \\(1\\)$", hits))
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
    paste0("Warning: ", fit$report$n_max_treedepth, " of 20 (",
           sprintf("%.1f", 100 * fit$report$n_max_treedepth / 20),
           "%) transitions hit the maximum treedepth limit of 1."),
    text, fixed = TRUE)))
  expect_true(any(grepl("See https://mc-stan.org/misc/warnings for details.",
                        text, fixed = TRUE)))
})

sampling_output <- function(code, data, ...) {
  low_ebfmi <- FALSE
  text <- capture.output({
    m <- stanli_model(code = code, data = data)
    fit <- sample_model(m, ...)
    low_ebfmi <- any(grepl("E-BFMI is below",
                           capture.output(stanli_diagnose(fit)), fixed = TRUE))
  })
  structure(paste(text, collapse = "\n"), low_ebfmi = low_ebfmi)
}

reject_code <- "
  parameters { real mu; }
  model {
    if (mu < 0) reject(\"mu must be >= 0, found mu = \", mu);
    mu ~ normal(0, 1);
  }"

funnel_code <- "
  parameters { real x; real y; }
  model { y ~ normal(0, 3); x ~ normal(0, exp(y / 2)); }"

test_that("rejected proposals are reported with their chain", {
  skip_without_runtime()
  loud <- sampling_output(reject_code, list(), chains = 2, seed = 3,
                          warmup = 100, samples = 100, refresh = 50)
  expect_match(loud, "Chain [1] Informational Message", fixed = TRUE)
  expect_match(loud, "Chain [2] Informational Message", fixed = TRUE)
  quiet <- sampling_output(reject_code, list(), chains = 2, seed = 3,
                           warmup = 100, samples = 100, refresh = 0)
  expect_false(grepl("Informational", quiet, fixed = TRUE))
})

test_that("divergence and low E-BFMI warnings come with every sampling run", {
  skip_without_runtime()
  loud <- sampling_output(funnel_code, list(), chains = 1, seed = 42,
                          warmup = 100, samples = 500, refresh = 1000)
  expect_match(loud, paste0(
    "Warning: [0-9]+ of 500 \\([0-9]+\\.[0-9]%\\) transitions ended with a ",
    "divergence\\.\nSee https://mc-stan.org/misc/warnings for details\\."))
  # The draws, and so the E-BFMI, differ across platforms' math libraries.
  expect_identical(grepl(paste0(
    "Warning: 1 of 1 chains had an E-BFMI less than 0.3.\n",
    "See https://mc-stan.org/misc/warnings for details."), loud, fixed = TRUE),
    attr(loud, "low_ebfmi"))
  quiet <- sampling_output(funnel_code, list(), chains = 1, seed = 42,
                           warmup = 100, samples = 500, refresh = 0)
  expect_false(grepl("Warning", quiet, fixed = TRUE))
})

test_that("threads_per_chain on a model without reduce_sum is noted once", {
  skip_without_runtime()
  skip_if_not(stanli_thread_safe())
  note <- paste0("Note: threads_per_chain = 2 has no effect on this model, ",
                 "which has no reduce_sum call to run in parallel.")
  loud <- sampling_output("parameters { real x; } model { x ~ normal(0, 1); }",
                          list(), chains = 2, warmup = 50, samples = 50,
                          refresh = 100, threads_per_chain = 2)
  expect_equal(lengths(regmatches(loud, gregexpr(note, loud, fixed = TRUE))), 1L)
  quiet <- sampling_output("parameters { real x; } model { x ~ normal(0, 1); }",
                           list(), chains = 2, warmup = 50, samples = 50,
                           refresh = 0, threads_per_chain = 2)
  expect_false(grepl(note, quiet, fixed = TRUE))
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

test_that("the run seed reaches the sampler", {
  skip_without_runtime()
  m <- progress_model()
  run <- function(seed) sample_model(m, chains = 1, seed = seed, warmup = 20,
                                     samples = 20, refresh = 0)$draws
  expect_identical(run(3), run(3))
  expect_false(identical(run(3), run(4)))
})

test_that("Pathfinder initialization starts each chain", {
  skip_without_runtime()
  options <- list(num_iterations = 100L, num_elbo_draws = 10L,
                  history_size = 5L, init_radius = 2)
  a <- sample_model(progress_model(), chains = 2, seed = 303, warmup = 30,
                    samples = 20, pathfinder_init = options, refresh = 0)
  expect_identical(dim(a$draws)[1:2], c(20L, 2L))
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

test_that("summary and diagnostics read the fit's draws", {
  skip_without_runtime()
  fit <- sample_model(es_model(), chains = 4, seed = 5, warmup = 300,
                      samples = 300, refresh = 0)
  s <- summary(fit)
  mu <- s[s$variable == "mu", ]
  expect_equal(mu$mean, mean(fit$draws[, , "mu"]), tolerance = 1e-12)
  expect_true(all(is.finite(s$rhat)) && all(is.finite(s$ess_bulk)))
  txt <- capture.output(stanli_diagnose(fit))
  expect_true(any(grepl("R-hat", txt)) && any(grepl("E-BFMI", txt)))
  named <- vapply(dimnames(fit$draws)[[3]], function(n)
    any(grepl(paste0(n, ")"), txt, fixed = TRUE)), logical(1))
  expect_true(any(named))
})

test_that("optimize finds a mode the sampler can start from", {
  skip_without_runtime()
  m <- es_model()
  o <- optimize_model(m, seed = 1)
  expect_length(o$unconstrained, m$n_unconstrained)
  # The reported lp must be the model's lp there, not the objective the
  # optimizer minimizes.
  expect_equal(o$lp, log_prob_grad(m, o$unconstrained)$lp, tolerance = 1e-8)
  fit <- sample_model(m, chains = 1, seed = 2, warmup = 20, samples = 20,
                      init = o$unconstrained, refresh = 0)
  expect_equal(dim(fit$draws)[1], 20L)
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

metric_model <- function() {
  stanli_model(code = "
    parameters { vector[2] x; }
    model { x ~ normal(0, [1, 3]'); }")
}

test_that("dense_e returns an SPD inverse metric and a step size per chain", {
  skip_without_runtime()
  fit <- sample_model(metric_model(), chains = 2, seed = 5, warmup = 300,
                      samples = 50, metric = "dense_e", refresh = 0)
  expect_identical(fit$metric, "dense_e")
  expect_length(fit$step_size, 2L)
  expect_true(all(is.finite(fit$step_size) & fit$step_size > 0))
  expect_length(fit$inv_metric, 2L)
  for (m in fit$inv_metric) {
    expect_true(is.matrix(m))
    expect_identical(dim(m), c(2L, 2L))
  }
})

test_that("diag_e and unit_e report what the sampler adapted", {
  skip_without_runtime()
  m <- metric_model()
  diag_fit <- sample_model(m, chains = 2, seed = 5, warmup = 300, samples = 50,
                           refresh = 0)
  expect_identical(diag_fit$metric, "diag_e")
  expect_length(diag_fit$inv_metric, 2L)
  for (v in diag_fit$inv_metric) {
    expect_true(is.numeric(v) && is.null(dim(v)))
    expect_length(v, 2L)
    expect_true(all(v > 0))
  }
  unit_fit <- sample_model(m, chains = 2, seed = 5, warmup = 100, samples = 20,
                           metric = "unit_e", refresh = 0)
  expect_identical(unit_fit$metric, "unit_e")
  expect_identical(unit_fit$inv_metric, list(NULL, NULL))
  expect_true(all(unit_fit$step_size > 0))
})

test_that("an adapted metric and step size feed a second fit", {
  skip_without_runtime()
  m <- metric_model()
  pilot <- sample_model(m, chains = 2, seed = 11, warmup = 300, samples = 30,
                        refresh = 0)
  eps <- mean(pilot$step_size)
  fit <- sample_model(m, chains = 2, seed = 12, warmup = 0, samples = 40,
                      inv_metric = pilot$inv_metric, step_size = eps,
                      adapt_engaged = FALSE, refresh = 0)
  expect_true(all(fit$sampler[, , "stepsize__"] == eps))
  expect_equal(fit$step_size, rep(eps, 2))
  expect_identical(fit$inv_metric, pilot$inv_metric)

  shared <- sample_model(m, chains = 2, seed = 12, warmup = 0, samples = 40,
                         inv_metric = pilot$inv_metric[[1]], step_size = eps,
                         adapt_engaged = FALSE, refresh = 0)
  expect_identical(shared$inv_metric,
                   list(pilot$inv_metric[[1]], pilot$inv_metric[[1]]))
  expect_true(all(shared$sampler[, , "stepsize__"] == eps))
})

test_that("a supplied dense metric round-trips with adaptation off", {
  skip_without_runtime()
  m <- metric_model()
  S <- matrix(c(2, 0.5, 0.5, 1.5), 2)
  fit <- sample_model(m, chains = 2, seed = 3, warmup = 0, samples = 20,
                      metric = "dense_e", inv_metric = S, step_size = 0.25,
                      adapt_engaged = FALSE, refresh = 0)
  expect_identical(fit$inv_metric, list(S, S))
  expect_true(all(fit$sampler[, , "stepsize__"] == 0.25))
})

test_that("adapt_metric = FALSE adapts only the step size", {
  skip_without_runtime()
  m <- metric_model()
  S <- matrix(c(2, 0.5, 0.5, 1.5), 2)
  fit <- sample_model(m, chains = 2, seed = 3, warmup = 300, samples = 20,
                      metric = "dense_e", inv_metric = S, adapt_metric = FALSE,
                      refresh = 0)
  expect_identical(fit$inv_metric, list(S, S))
  expect_false(fit$adapt_metric)
})

test_that("warmup = 0 searches for a step size only while adaptation is engaged", {
  skip_without_runtime()
  m <- metric_model()
  searched <- sample_model(m, chains = 1, seed = 3, warmup = 0, samples = 10,
                           step_size = 0.25, refresh = 0)
  expect_false(all(searched$sampler[, , "stepsize__"] == 0.25))
  fixed <- sample_model(m, chains = 1, seed = 3, warmup = 0, samples = 10,
                        step_size = 0.25, adapt_engaged = FALSE, refresh = 0)
  expect_true(all(fixed$sampler[, , "stepsize__"] == 0.25))
})

test_that("a length-one inverse metric fits a one-parameter model", {
  skip_without_runtime()
  fit <- sample_model(progress_model(), chains = 2, seed = 3, warmup = 0,
                      samples = 10, inv_metric = 4, step_size = 0.5,
                      adapt_engaged = FALSE, refresh = 0)
  expect_identical(fit$inv_metric, list(4, 4))
  dense <- sample_model(progress_model(), chains = 1, seed = 3, warmup = 0,
                        samples = 10, metric = "dense_e", inv_metric = 4,
                        step_size = 0.5, adapt_engaged = FALSE, refresh = 0)
  expect_identical(dense$inv_metric, list(matrix(4, 1, 1)))
})

test_that("inverse metric shapes are checked before sampling", {
  skip_without_runtime()
  m <- metric_model()
  run <- function(...) sample_model(m, chains = 2, warmup = 10, samples = 10,
                                    refresh = 0, ...)
  expect_error(run(inv_metric = c(1, 1, 1)), "length 2")
  expect_error(run(inv_metric = matrix(1, 2, 2)), "length 2")
  expect_error(run(metric = "dense_e", inv_metric = c(1, 1)), "2 x 2")
  expect_error(run(metric = "dense_e", inv_metric = matrix(1, 2, 3)), "2 x 2")
  expect_error(run(inv_metric = list(c(1, 1), c(1, 1), c(1, 1))),
               "one element per chain")
  expect_error(run(inv_metric = list(c(1, 1), c(1, 1, 1))), "length 2")
  expect_error(run(metric = "unit_e", inv_metric = c(1, 1)), "unit_e")
  expect_error(run(inv_metric = c(1, NA)), "finite")
  expect_error(run(inv_metric = "a"), "numeric")
  expect_error(run(metric = "full_e"), "should be one of")
})

test_that("a non positive definite inverse metric surfaces Stan's message", {
  skip_without_runtime()
  m <- metric_model()
  expect_error(
    sample_model(m, chains = 2, warmup = 10, samples = 10, refresh = 0,
                 metric = "dense_e", inv_metric = matrix(c(1, 2, 2, 1), 2)),
    "^Inverse Euclidean metric not positive definite")
  expect_error(
    sample_model(m, chains = 2, warmup = 10, samples = 10, refresh = 0,
                 inv_metric = c(1, -1)),
    "positive")
})

test_that("step_size and adapt_engaged are validated", {
  skip_without_runtime()
  m <- metric_model()
  run <- function(...) sample_model(m, chains = 2, warmup = 10, samples = 10,
                                    refresh = 0, ...)
  for (bad in list(0, -1, NA_real_, Inf, "a", NULL))
    if (!is.null(bad)) expect_error(run(step_size = bad), "step_size")
  expect_error(run(step_size = c(0.1, 0.2)), "step_size")
  expect_error(run(adapt_engaged = NA), "adapt_engaged")
  expect_error(run(adapt_engaged = c(TRUE, FALSE)), "adapt_engaged")
  expect_error(run(adapt_metric = NA), "adapt_metric")
  expect_error(run(adapt_metric = c(TRUE, FALSE)), "adapt_metric")
  expect_error(run(adapt_metric = "no"), "adapt_metric")
})

test_that("fixed_param refuses metric options", {
  skip_without_runtime()
  m <- progress_model()
  for (args in list(list(metric = "dense_e"), list(inv_metric = 1),
                    list(step_size = 0.5), list(adapt_engaged = FALSE),
                    list(adapt_metric = FALSE)))
    expect_error(do.call(sample_model, c(list(m, chains = 1, samples = 5,
                                              fixed_param = TRUE, refresh = 0),
                                         args)),
                 "fixed_param")
})

chain_id_model <- function() {
  stanli_model(code = "
    parameters { real mu; real<lower=0> tau; }
    model { mu ~ normal(0, 3); tau ~ lognormal(0, 1); }
    generated quantities { real draw = normal_rng(mu, tau); }")
}

test_that("four chains equal four one-chain runs started at chain ids 1 to 4", {
  skip_without_runtime()
  m <- chain_id_model()
  args <- list(m, seed = 11, warmup = 60, samples = 40, refresh = 0)
  together <- do.call(sample_model, c(args, list(chains = 4)))
  for (k in 1:4) {
    alone <- do.call(sample_model, c(args, list(chains = 1, chain_id = k)))
    expect_identical(alone$draws[, 1, ], together$draws[, k, ])
    expect_identical(alone$sampler[, 1, ], together$sampler[, k, ])
    expect_identical(alone$unconstrained[, 1, ], together$unconstrained[, k, ])
    expect_identical(alone$step_size, together$step_size[k])
    expect_identical(alone$chain_id, as.integer(k))
  }
  expect_identical(together$chain_id, 1L)
})

test_that("chain_id continues across chains and defaults to 1", {
  skip_without_runtime()
  m <- chain_id_model()
  fit <- sample_model(m, chains = 3, chain_id = 5, seed = 2, warmup = 20,
                      samples = 10, refresh = 0)
  expect_identical(fit$chain_id, 5L)
  tail <- sample_model(m, chains = 2, chain_id = 7, seed = 2, warmup = 20,
                       samples = 10, refresh = 0)
  expect_identical(fit$draws[, 3, ], tail$draws[, 1, ])
})

test_that("chain_id is validated", {
  skip_without_runtime()
  m <- chain_id_model()
  for (bad in list(0, -1, 1.5, NA, NA_integer_, c(1, 2), "1", Inf, TRUE,
                   .Machine$integer.max))
    expect_error(sample_model(m, chains = 2, chain_id = bad, warmup = 5,
                              samples = 5, refresh = 0), "chain_id")
})

test_that("progress and timing report the chain ids in use", {
  skip_without_runtime()
  m <- chain_id_model()
  text <- capture.output(
    sample_model(m, chains = 2, chain_id = 3, seed = 7, warmup = 2,
                 samples = 2, init_radius = 0, parallel_chains = 1,
                 refresh = 1))
  expect_true(any(grepl("^Chain \\[3\\] Iteration:", text)))
  expect_true(any(grepl("^Chain \\[4\\] Iteration:", text)))
  expect_false(any(grepl("^Chain \\[[12]\\] ", text)))
  expect_true(any(grepl("^Chain \\[4\\] Elapsed Time", text)))
})

test_that("fixed_param chains follow chain_id", {
  skip_without_runtime()
  m <- chain_id_model()
  together <- sample_model(m, fixed_param = TRUE, chains = 3, seed = 4,
                           samples = 10, refresh = 0)
  alone <- sample_model(m, fixed_param = TRUE, chains = 1, chain_id = 3,
                        seed = 4, samples = 10, refresh = 0)
  expect_identical(alone$draws[, 1, ], together$draws[, 3, ])
  expect_identical(alone$chain_id, 3L)
})

test_that("Pathfinder starts are keyed on the first chain id", {
  skip_without_runtime()
  m <- chain_id_model()
  fit <- sample_model(m, chains = 1, chain_id = 2, seed = 5, warmup = 30,
                      samples = 10, refresh = 0, pathfinder_init = list())
  starts <- .Call("stanli_r_pathfinder_inits", m$ptr, 5L, 1L,
                  unname(stanli:::.pathfinder_init_options(list())), 2L)
  explicit <- sample_model(m, chains = 1, chain_id = 2, seed = 5, warmup = 30,
                           samples = 10, refresh = 0, init = starts)
  expect_identical(fit$draws, explicit$draws)
})

test_that("sampling never touches the caller's random stream", {
  skip_without_runtime()
  m <- chain_id_model()
  set.seed(21)
  before <- random_state()
  sample_model(m, chains = 2, seed = 3, warmup = 10, samples = 5, refresh = 0)
  sample_model(m, chains = 2, seed = 3, warmup = 10, samples = 5, refresh = 0,
               pathfinder_init = list())
  sample_model(m, chains = 2, seed = 3, samples = 5, refresh = 0,
               fixed_param = TRUE)
  expect_identical(random_state(), before)
  without_random_state({
    sample_model(m, chains = 1, seed = 3, warmup = 10, samples = 5,
                 refresh = 0)
    expect_null(random_state())
  })
})
