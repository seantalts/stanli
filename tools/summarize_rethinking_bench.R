#!/usr/bin/env Rscript
# Post-run diagnostics only; never run concurrently with the timed sweep.
args <- commandArgs(TRUE)
stopifnot(length(args) == 2L)
jobs <- jsonlite::fromJSON(args[1], simplifyVector = FALSE)
results <- list()
for (job in jobs) {
  engines <- list()
  summaries <- list()
  for (engine in c("stanli", "cmdstan")) {
    files <- unlist(job$files[[engine]])
    if (length(files) != 4L) {
      engines[[engine]] <- list(status = "incomplete", chains = length(files))
      next
    }
    wanted <- unlist(job$columns)
    constants <- unlist(job$constants)
    chains <- lapply(files, function(path) {
      con <- file(path, "r"); on.exit(close(con))
      repeat {
        line <- readLines(con, n = 1L)
        if (!length(line)) stop("missing CSV header: ", path)
        if (nzchar(line) && !startsWith(line, "#")) break
      }
      names <- strsplit(line, ",", fixed = TRUE)[[1]]
      keep <- names %in% c(wanted, names(constants), "divergent__", "treedepth__", "energy__")
      x <- read.csv(path, comment.char = "#", check.names = FALSE,
                    colClasses = ifelse(keep, "numeric", "NULL"))
      stopifnot(nrow(x) == 1000L, all(wanted %in% names(x)))
      for (column in names(constants))
        stopifnot(all(abs(x[[column]] - constants[[column]]) <= 1e-12))
      x
    })
    draws <- array(NA_real_, c(1000L, 4L, length(wanted)),
                   dimnames = list(NULL, NULL, wanted))
    for (i in 1:4) draws[, i, ] <- as.matrix(chains[[i]][wanted])
    summary <- posterior::summarise_draws(posterior::as_draws_array(draws),
      "mean", "sd", "rhat", "ess_bulk", "ess_tail", "mcse_mean")
    summaries[[engine]] <- summary
    divergences <- sum(vapply(chains, function(x) sum(x$divergent__), numeric(1)))
    depth <- sum(vapply(chains, function(x) sum(x$treedepth__ >= 10), numeric(1)))
    ebfmi <- vapply(chains, function(x) mean(diff(x$energy__)^2) / var(x$energy__), numeric(1))
    finite <- function(x, fun) if (any(is.finite(x))) fun(x[is.finite(x)]) else NA_real_
    engines[[engine]] <- list(status = "complete", chains = 4L,
      parameters = length(wanted), fixed_entries_omitted = length(constants),
      draws = 4000L, divergences = divergences,
      max_depth_hits = depth, rhat_max = finite(summary$rhat, max),
      ess_bulk_min = finite(summary$ess_bulk, min), ess_tail_min = finite(summary$ess_tail, min),
      rhat_over_1_01 = sum(summary$rhat > 1.01, na.rm = TRUE),
      undefined_diagnostics = sum(!is.finite(summary$rhat) | !is.finite(summary$ess_bulk)),
      ebfmi_min = finite(ebfmi, min),
      screening_flag = divergences > 0 || depth > 0 ||
        any(!is.finite(summary$rhat) | summary$rhat > 1.01 | !is.finite(summary$ess_bulk) | summary$ess_bulk < 400))
  }
  comparison <- list(status = "incomplete")
  if (length(summaries) == 2L) {
    a <- summaries$stanli; b <- summaries$cmdstan
    stopifnot(identical(a$variable, b$variable))
    pooled_sd <- sqrt((a$sd^2 + b$sd^2) / 2)
    scaled <- abs(a$mean - b$mean) / pooled_sd
    valid <- is.finite(scaled)
    comparison <- list(status = "complete", parameters = length(scaled),
      median_mean_difference_in_sd = median(scaled[valid]),
      max_mean_difference_in_sd = max(scaled[valid]),
      max_difference_parameter = a$variable[which.max(replace(scaled, !valid, -Inf))])
  }
  results[[job$model]] <- list(engines = engines, comparison = comparison)
  cat(job$model, "diagnostics complete\n")
}
jsonlite::write_json(results, args[2], auto_unbox = TRUE, pretty = TRUE,
                     digits = 16, na = "null")
