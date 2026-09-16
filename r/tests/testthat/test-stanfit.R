# The candidate is always built directly from R arrays. CSV is an independent
# oracle only: numbers and headers come exclusively from stanli_run.
stanfit_cli_oracle <- function(code, warmup, samples, thin, save_warmup,
                               chains = 2L, seed = 18L) {
  cli <- Sys.getenv("STANLI_RUN", Sys.which("stanli_run"))
  if (!nzchar(cli) || !file.exists(cli)) skip("no stanli_run for CSV oracle")
  directory <- tempfile("stanfit-oracle-")
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  model_file <- file.path(directory, "shape_model.stan")
  data_file <- file.path(directory, "data.json")
  csv_file <- file.path(directory, "raw.csv")
  error_file <- file.path(directory, "stderr.txt")
  writeLines(code, model_file)
  writeLines("{}", data_file)
  flags <- c(shQuote(model_file), shQuote(data_file), "--seed", seed,
             "--warmup", warmup, "--samples", samples, "--thin", thin,
             "--chains", chains, "--sampler-stats")
  if (save_warmup) flags <- c(flags, "--save-warmup")
  status <- system2(cli, flags, stdout = csv_file, stderr = error_file)
  expect_equal(status, 0L, info = paste(readLines(error_file), collapse = "\n"))
  raw <- readLines(csv_file)
  rows <- ceiling(samples / thin) + if (save_warmup) ceiling(warmup / thin) else 0L
  expect_length(raw, 1L + rows * chains)
  files <- file.path(directory, paste0("shape_model_", seq_len(chains), ".csv"))
  for (chain in seq_len(chains)) {
    # The current CLI concatenates chains and emits no configuration comments.
    # Add only metadata from the invocation above, and split without parsing or
    # rewriting numeric rows. No candidate fit contributes to this reference.
    metadata <- c(paste0("# num_samples = ", samples),
                  paste0("# num_warmup = ", warmup), paste0("# thin = ", thin),
                  paste0("# save_warmup = ", as.integer(save_warmup)),
                  paste0("# seed = ", seed), paste0("# id = ", chain),
                  "# algorithm = hmc", "# engine = nuts", "# metric = diag_e", "#")
    body <- raw[1L + (chain - 1L) * rows + seq_len(rows)]
    writeLines(c(metadata, raw[1L], "# Adaptation terminated", body,
                 "# Elapsed Time: NA seconds (Warm-up)",
                 "#               NA seconds (Sampling)"), files[chain])
  }
  set.seed(104)
  rstan::read_stan_csv(files)
}

# The native conversion preserves IEEE doubles exactly. Decimal CSV parsing
# can round differently by one ULP (also reproducible with R's own sprintf /
# as.numeric), so bound each oracle value separately at two machine epsilons.
expect_csv_numbers <- function(actual, reference) {
  expect_identical(lapply(actual, attributes), lapply(reference, attributes))
  a <- as.numeric(unlist(actual))
  b <- as.numeric(unlist(reference))
  expect_length(a, length(b))
  expect_true(all(abs(a - b) <= 2 * .Machine$double.eps *
                    pmax(abs(b), .Machine$double.xmin)),
              info = "CLI CSV values differ beyond decimal round-trip precision")
}

stanfit_shape_code <- "
  parameters {
    real s; vector[2] v; matrix[2,3] M;
    array[2] vector[3] av; array[2] matrix[2,3] am;
    simplex[3] p;
  }
  model {
    s ~ std_normal(); v ~ std_normal(); to_vector(M) ~ std_normal();
    p ~ dirichlet(rep_vector(2, 3));
    for (i in 1:2) { av[i] ~ std_normal(); to_vector(am[i]) ~ std_normal(); }
  }
  generated quantities { real doubled = 2 * s; real y_rep = normal_rng(s, 1); }
"

test_that("direct stanfit construction agrees with rstan's CLI CSV import", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  m <- stanli_model(code = stanfit_shape_code)
  for (save_warmup in c(FALSE, TRUE)) {
    fit <- sample_model(m, chains = 2, seed = 18, warmup = 101,
                        samples = 105, thin = 3, save_warmup = save_warmup,
                        refresh = 0)
    reference <- stanfit_cli_oracle(stanfit_shape_code, 101, 105, 3, save_warmup)
    set.seed(104)
    actual <- as_stanfit(fit)
    expect_s4_class(actual, "stanfit")
    expect_true(methods::validObject(actual))
    expect_identical(actual@model_pars, reference@model_pars)
    expect_identical(actual@par_dims, reference@par_dims)
    expect_identical(names(actual), names(reference))
    expect_equal(dim(actual), dim(reference))
    expect_identical(dimnames(as.array(actual)), dimnames(as.array(reference)))
    expect_csv_numbers(as.array(actual), as.array(reference))
    expect_identical(dimnames(as.matrix(actual)), dimnames(as.matrix(reference)))
    expect_csv_numbers(as.matrix(actual), as.matrix(reference))
    # Every original double survives conversion, including saved warmup and lp.
    stored <- rstan::extract(actual, permuted = FALSE, inc_warmup = TRUE)
    for (column in seq_along(fit$columns))
      expect_identical(unname(stored[, , column]), unname(fit$draws[, , column]))
    expect_identical(unname(stored[, , "lp__"]), unname(fit$sampler[, , "lp__"]))
    # Permutations depend on the R seed, not on the sampler seed.
    expect_identical(actual@sim$permutation, reference@sim$permutation)
    expect_csv_numbers(rstan::extract(actual), rstan::extract(reference))
    expect_equal(rstan::summary(actual)$summary,
                 rstan::summary(reference)$summary, tolerance = 1e-12)
    expect_equal(rstan::get_sampler_params(actual, inc_warmup = FALSE),
                 rstan::get_sampler_params(reference, inc_warmup = FALSE),
                 tolerance = 1e-14)
    expect_equal(rstan::get_posterior_mean(actual),
                 rstan::get_posterior_mean(reference), tolerance = 1e-14)
    if (save_warmup) {
      expect_equal(rstan::extract(actual, permuted = FALSE, inc_warmup = TRUE),
                   rstan::extract(reference, permuted = FALSE, inc_warmup = TRUE),
                   tolerance = 1e-14)
      expect_equal(rstan::get_sampler_params(actual),
                   rstan::get_sampler_params(reference), tolerance = 1e-14)
    } else {
      expect_warning(rstan::get_sampler_params(actual), "warmup samples not saved")
      expect_warning(rstan::get_sampler_params(reference), "warmup samples not saved")
    }
    for (name in c("iter", "thin", "warmup", "chains", "n_save", "warmup2",
                   "pars_oi", "dims_oi", "fnames_oi", "n_flatnames"))
      expect_equal(actual@sim[[name]], reference@sim[[name]], info = name)
    elapsed <- cbind(warmup = fit$report$warmup_seconds,
                     sample = fit$report$sampling_seconds)
    rownames(elapsed) <- paste0("chain:", 1:2)
    expect_identical(rstan::get_elapsed_time(actual), elapsed)
    # CSV-only objects lack a counter; our fit retains its exact size.
    expect_error(rstan::get_num_upars(reference), "model object.*not valid")
    expect_identical(rstan::get_num_upars(actual), dim(fit$unconstrained)[3L])
  }
})

test_that("source, configuration, and missing timings survive conversion", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  file <- tempfile("named_model", fileext = ".stan")
  on.exit(unlink(file))
  code <- "parameters { real mu; } model { mu ~ normal(0, 1); }"
  writeLines(code, file)
  model <- stanli_model(file)
  fit <- sample_model(model, chains = 1, warmup = 11, samples = 13,
                      thin = 2, save_warmup = TRUE, seed = 7, delta = 0.9,
                      max_depth = 8, refresh = 0)
  sf <- as_stanfit(fit)
  expect_identical(sf@model_name, tools::file_path_sans_ext(basename(file)))
  expect_match(sf@stanmodel@model_code, code, fixed = TRUE)
  expect_identical(sf@stan_args[[1]]$control,
                   list(adapt_delta = 0.9, max_treedepth = 8))
  expect_equal(sf@stan_args[[1]][c("iter", "warmup", "thin", "seed", "chain_id")],
               list(iter = 24, warmup = 11, thin = 2, seed = 7, chain_id = 1))
  expect_identical(sf@stan_args[[1]]$method, "sampling")
  expect_identical(sf@stan_args[[1]]$algorithm, "NUTS")
  expect_true(sf@stan_args[[1]]$save_warmup)
  expect_length(sf@inits, 0L)
  saved <- tempfile(fileext = ".rds")
  on.exit(unlink(saved), add = TRUE)
  saveRDS(sf, saved)
  restored <- readRDS(saved)
  expect_true(methods::validObject(restored))
  expect_identical(as.array(restored), as.array(sf))
  expect_identical(rstan::get_sampler_params(restored), rstan::get_sampler_params(sf))
  fit$report$available <- FALSE
  fit$model <- NULL
  expect_true(all(is.na(rstan::get_elapsed_time(as_stanfit(fit)))))
  expect_identical(as_stanfit(fit)@model_name, "stanli_model")
  fit$thin <- NULL
  expect_error(as_stanfit(fit), "lacks sampling metadata")
})

test_that("serialized model-dependent calls fail safely without a live handle", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("callr")
  fit <- sample_model(es_model(), chains = 1, warmup = 20, samples = 20, refresh = 0)
  sf <- as_stanfit(fit)
  # A subprocess turns a native crash into a test failure, not a lost test run.
  messages <- callr::r(function(sf) {
    functions <- list(
      function() rstan::log_prob(sf, rep(0, 10)),
      function() rstan::grad_log_prob(sf, rep(0, 10)),
      function() rstan::unconstrain_pars(sf, list(mu = 0)),
      function() rstan::constrain_pars(sf, rep(0, 10))
    )
    lapply(functions, function(f) tryCatch({f(); "unexpected success"},
                                         error = conditionMessage))
  }, list(sf), libpath = .libPaths())
  for (message in messages) expect_match(message, "no live stanli model")
  expect_identical(rstan::get_num_upars(sf), 10L)
})

test_that("rstan, bayesplot and loo consume the native in-memory fit", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("bayesplot")
  skip_if_not_installed("loo")
  fit <- sample_model(log_lik_model(), chains = 4, warmup = 500, samples = 1000,
                      refresh = 0)
  sf <- as_stanfit(fit)
  expect_s3_class(bayesplot::mcmc_trace(sf, pars = "mu"), "ggplot")
  expect_s3_class(rstan::traceplot(sf, pars = "mu"), "ggplot")
  expect_s3_class(rstan::stan_dens(sf, pars = "mu"), "ggplot")
  expect_s3_class(rstan::stan_plot(sf, pars = "mu"), "ggplot")
  expected <- loo::loo(fit)
  actual <- loo::loo(sf)
  expect_s3_class(actual, "psis_loo")
  expect_equal(actual$estimates["elpd_loo", ], expected$estimates["elpd_loo", ],
               tolerance = 1e-12)
})

test_that("shinystan imports a stanfit without compiling a model", {
  skip_without_runtime()
  skip_if_not_installed("rstan")
  skip_if_not_installed("shinystan")
  fit <- sample_model(es_model(), chains = 4, warmup = 500, samples = 500,
                      refresh = 0)
  expect_s4_class(shinystan::as.shinystan(as_stanfit(fit)), "shinystan")
})
