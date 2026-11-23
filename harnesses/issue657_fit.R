# Matched settings, synthetic workloads: one backend/seed/case per process.
# Rscript harnesses/issue657_fit.R BACKEND CASE_DIR DATA.json OUT WARMUP DRAWS SEED
# CmdStan executable CASE_DIR/proxy must already exist; no shared builds occur.
# STANLI_RUNTIME and R_LIBS select the exact runtime and private source wrapper.
args <- commandArgs(trailingOnly = TRUE)
stopifnot(length(args) == 7L, args[1L] %in% c("stanli", "cmdstan"))
backend <- args[1L]
case <- normalizePath(args[2L])
data <- normalizePath(args[3L])
out <- args[4L]
dir.create(out, recursive = TRUE, showWarnings = FALSE)
warmup <- as.integer(args[5L])
draws_n <- as.integer(args[6L])
seed <- as.integer(args[7L])
chains <- as.integer(Sys.getenv("ISSUE657_CHAINS", "2"))
stopifnot(!is.na(chains), chains >= 2L)
clock <- function() as.numeric(Sys.time())
stopifnot(requireNamespace("posterior"), requireNamespace("jsonlite"))
warnings <- character()
t0 <- clock()
if (backend == "stanli") {
  stopifnot(nzchar(Sys.getenv("STANLI_RUNTIME")), requireNamespace("stanli"))
  model <- stanli::stanli_model(file = file.path(case, "proxy.stan"), data = data,
                                allow_interpreter = FALSE)
} else {
  stopifnot(requireNamespace("cmdstanr"), nzchar(Sys.getenv("CMDSTAN")))
  cmdstanr::set_cmdstan_path(Sys.getenv("CMDSTAN"))
  model <- cmdstanr::cmdstan_model(stan_file = file.path(case, "proxy.stan"),
                                   exe_file = file.path(case, "proxy"),
                                   compile = FALSE)
}
setup_s <- clock() - t0
t0 <- clock()
fit <- withCallingHandlers({
  if (backend == "stanli") {
    stanli::sample_model(model, chains = chains, parallel_chains = 1, seed = seed,
                          warmup = warmup, samples = draws_n,
                          init_radius = 0, delta = 0.8, max_depth = 10,
                          save_warmup = TRUE, refresh = 0,
                          show_messages = FALSE)
  } else {
    model$sample(data = data, chains = chains, parallel_chains = 1, seed = seed,
                   iter_warmup = warmup, iter_sampling = draws_n, init = 0,
                   adapt_delta = 0.8, max_treedepth = 10, save_warmup = TRUE,
                   refresh = 0, sig_figs = 17, output_dir = out)
  }
}, warning = function(w) {
  warnings <<- c(warnings, conditionMessage(w))
  invokeRestart("muffleWarning")
})
fit_s <- clock() - t0
t0 <- clock()
if (backend == "stanli") {
  draws <- stanli::as_draws_array(fit)
  diagnostics <- fit$sampler
  engine_times <- fit$report
  metadata <- list(build_id = stanli::stanli_build_id(),
                    columns = fit$columns, runtime = Sys.getenv("STANLI_RUNTIME"))
  csv_bytes <- 0
  chosen <- unique(c(warmup + 1L, warmup + ceiling(draws_n / 2),
                     warmup + draws_n))
  points <- lapply(chosen, function(i) as.numeric(fit$unconstrained[i, 1L, ]))
  jsonlite::write_json(points, file.path(out, "posterior-points.json"),
                         auto_unbox = TRUE, digits = 17)
} else {
  draws <- fit$draws(format = "draws_array")
  diagnostics <- fit$sampler_diagnostics(inc_warmup = TRUE, format = "draws_array")
  engine_times <- fit$time()
  metadata <- fit$metadata()
  csv_bytes <- sum(file.info(fit$output_files())$size)
}
read_s <- clock() - t0
post <- seq.int(warmup + 1L, dim(diagnostics)[1L])
metric <- function(name, rows = seq_len(dim(diagnostics)[1L])) {
  sum(diagnostics[rows, , name])
}
# lp__ is absent from Stanli's draw array; compare the actual model columns.
draws <- posterior::subset_draws(draws, variable = setdiff(
  posterior::variables(draws), c("lp__", "lprior")))
summary <- posterior::summarise_draws(draws, "mean", "sd", "mcse_mean",
                                      "rhat", "ess_bulk", "ess_tail")
utils::write.table(summary, file.path(out, "summary.tsv"), sep = "\t",
                     quote = FALSE, row.names = FALSE)
saveRDS(list(draws = draws, diagnostics = diagnostics), file.path(out, "draws.rds"))
result <- list(backend = backend, seed = seed, setup_s = setup_s,
                 sample_call_s = fit_s, csv_read_or_array_extract_s = read_s,
                 warmup = warmup, samples = draws_n, chains = chains,
                 parallel_chains = 1, init_radius = 0, delta = 0.8,
                 max_depth = 10, save_warmup = TRUE, sig_figs_cmdstan = 17,
                 total_leapfrogs = metric("n_leapfrog__"),
                 post_leapfrogs = metric("n_leapfrog__", post),
                 post_divergences = metric("divergent__", post),
                 post_depth_hits = sum(diagnostics[post, , "treedepth__"] >= 10),
                 max_rhat = max(summary$rhat, na.rm = TRUE),
                 min_bulk_ess = min(summary$ess_bulk, na.rm = TRUE),
                 finite_draws = all(is.finite(draws)),
                 output_columns = dim(draws)[3L],
                 retained_draw_bytes = as.numeric(object.size(draws)),
                 csv_bytes = csv_bytes, engine_times = engine_times,
                 metadata = metadata, warnings = warnings)
jsonlite::write_json(result, file.path(out, "result.json"), auto_unbox = TRUE,
                       digits = 17, pretty = TRUE, na = "null", null = "null")
cat(backend, "sample_call_s", fit_s, "read_s", read_s,
      "leapfrogs", result$total_leapfrogs, "rhat", result$max_rhat,
      "min ESS", result$min_bulk_ess, "\n")
