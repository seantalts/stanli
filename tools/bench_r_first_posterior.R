# The parent measures from launching Rscript --vanilla to the first posterior,
# including process startup, package/runtime loading, compilation, and NUTS.
# Installation/download and plotting are excluded; no model cache is reused.
args <- commandArgs(trailingOnly = TRUE)
if (identical(args, "--child")) {
  library(stanli)
  m <- stanli_model(code = "
    data { int J; vector[J] y; vector[J] sigma; }
    parameters { real mu; real<lower=0> tau; vector[J] z; }
    transformed parameters { vector[J] theta = mu + tau * z; }
    model { mu ~ normal(0, 5); tau ~ cauchy(0, 5);
            z ~ std_normal(); y ~ normal(theta, sigma); }",
    data = list(J = 8L, y = c(28, 8, -3, 7, -1, 1, 18, 12),
                sigma = c(15, 10, 16, 11, 9, 11, 10, 18)))
  fit <- sample_model(m, chains = 4, seed = 1, warmup = 1000,
                      samples = 1000, refresh = 0)
  stopifnot(identical(dim(fit$draws)[1:2], c(1000L, 4L)),
            all(is.finite(fit$draws)))
} else {
  script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE))
  elapsed <- replicate(3L, {
    timing <- system.time(status <- system2(
      file.path(R.home("bin"), "Rscript"),
      c("--vanilla", shQuote(normalizePath(script)), "--child")))
    stopifnot(status == 0L)
    unname(timing[["elapsed"]])
  })
  result <- data.frame(os = Sys.info()[["sysname"]], arch = R.version$arch,
                       r = as.character(getRversion()),
                       stanli = as.character(packageVersion("stanli")),
                       run = seq_along(elapsed), elapsed_seconds = elapsed)
  output <- if (length(args)) args[[1L]] else "r-first-posterior.csv"
  utils::write.csv(result, output, row.names = FALSE)
  print(result)
  cat("Median fresh-session wall time:", median(elapsed), "seconds\n")
  summary <- Sys.getenv("GITHUB_STEP_SUMMARY")
  if (nzchar(summary))
    cat("\n### R: fresh session to first posterior\n\n",
        result$os[[1]], result$arch[[1]], "— median", median(elapsed),
        "s (three fresh sessions, 4 chains, 1000 warmup + 1000 draws).\n",
        file = summary, append = TRUE)
}
