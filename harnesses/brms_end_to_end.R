# Fits every brms model shape in tests/brms through brms itself, end to end,
# with one backend, and records what happened to each.
#
# tests/brms holds the Stan code and data brms generates for 124 formulas;
# those are checked against CmdStan value by value. This asks the other
# question: does brm() with that backend return a brmsfit that the usual
# post-processing accepts? The formulas are read from tools/gen_brms_models.R,
# so the two cannot drift.
#
#   Rscript harnesses/brms_end_to_end.R BACKEND OUT.tsv [SLUG_REGEX]
#
# Beside OUT.tsv it writes OUT.summaries.rds: per model, the posterior mean,
# standard deviation and Monte Carlo standard error of every variable, for
# comparing two backends (harnesses/brms_compare_backends.R).
# BRMS_E2E_SHARD=k/n runs every n-th case starting at the k-th, so a backend
# that compiles each model can be spread over processes.
#
# Needs brms with the backend, mgcv, mice and splines2. A backend other than
# "stanli" needs its own toolchain.

suppressMessages({library(brms); library(mgcv)})
args <- commandArgs(trailingOnly = TRUE)
backend <- args[1]
out_path <- args[2]
only <- if (length(args) >= 3) args[3] else "."
chains <- 4L
iter <- 2000L
shard <- as.integer(strsplit(Sys.getenv("BRMS_E2E_SHARD", "1/1"), "/")[[1L]])
seen <- 0L
summaries <- list()

rows <- list()
record <- function(...) rows[[length(rows) + 1L]] <<- data.frame(..., stringsAsFactors = FALSE)

step <- function(expr) {
  tryCatch({force(expr); "ok"},
           error = function(e) paste("ERR:", gsub("\\s+", " ", conditionMessage(e))))
}

case <- function(slug, ...) {
  if (!grepl(only, slug)) return(invisible())
  seen <<- seen + 1L
  if ((seen - 1L) %% shard[2L] != shard[1L] - 1L) return(invisible())
  args <- list(...)
  fit_args <- c(args, list(backend = backend, chains = chains, iter = iter,
                           seed = 20261009L, silent = 2, refresh = 0))
  warned <- character()
  started <- Sys.time()
  fit <- withCallingHandlers(
    tryCatch(do.call(brm, fit_args), error = function(e) e),
    warning = function(w) {
      warned <<- c(warned, gsub("\\s+", " ", conditionMessage(w)))
      invokeRestart("muffleWarning")
    })
  seconds <- as.numeric(difftime(Sys.time(), started, units = "secs"))
  if (inherits(fit, "error")) {
    record(slug = slug, fit = paste("ERR:", gsub("\\s+", " ", conditionMessage(fit))),
           seconds = seconds, max_rhat = NA_real_, divergent = NA_integer_,
           summary = NA, draws = NA, predict = NA, log_lik = NA,
           warnings = paste(unique(warned), collapse = " | "))
    cat(sprintf("%-28s FIT ERROR\n", slug))
    return(invisible())
  }
  summaries[[slug]] <<- tryCatch(
    as.data.frame(suppressWarnings(posterior::summarise_draws(
      posterior::as_draws_df(fit), "mean", "sd", "mcse_mean", "rhat", "ess_bulk"))),
    error = function(e) NULL)
  rhats <- suppressWarnings(tryCatch(brms::rhat(fit), error = function(e) NA_real_))
  np <- tryCatch(brms::nuts_params(fit), error = function(e) NULL)
  divergent <- if (is.null(np)) NA_integer_ else
    as.integer(sum(np$Value[np$Parameter == "divergent__"]))
  record(
    slug = slug, fit = "ok", seconds = seconds,
    max_rhat = suppressWarnings(max(rhats, na.rm = TRUE)),
    divergent = divergent,
    summary = step(suppressWarnings(summary(fit))),
    draws = step(posterior::as_draws_df(fit)),
    predict = step(suppressWarnings(posterior_predict(fit, ndraws = 20))),
    log_lik = step(suppressWarnings(log_lik(fit, ndraws = 20))),
    warnings = paste(unique(warned), collapse = " | "))
  cat(sprintf("%-28s ok %6.2fs\n", slug, seconds))
}

# Everything in the generator but its own definition of case() and its output
# directory handling.
exprs <- parse("tools/gen_brms_models.R")
skip <- function(e) {
  is.call(e) && identical(e[[1L]], as.name("<-")) &&
    as.character(e[[2L]])[1L] %in% c("case", "OUT", "args")
}
OUT <- tempdir()
for (e in exprs) {
  if (skip(e)) next
  if (is.call(e) && identical(e[[1L]], as.name("dir.create"))) next
  if (is.call(e) && identical(e[[1L]], as.name("suppressMessages"))) next
  eval(e, envir = globalenv())
}
result <- do.call(rbind, rows)
utils::write.table(result, out_path, sep = "\t", quote = FALSE, row.names = FALSE)
saveRDS(summaries, sub("\\.tsv$", ".summaries.rds", out_path))
cat(sprintf("%d cases: %d fit, %d failed\n", nrow(result),
            sum(result$fit == "ok"), sum(result$fit != "ok")))
