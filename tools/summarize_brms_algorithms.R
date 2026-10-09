# Turns the output of tools/compare_brms_algorithms.R into the tables of
# notes/execution/2026-10-09-other-inference-algorithms.md.
#
#   Rscript tools/summarize_brms_algorithms.R OUT.csv
#
# For every run of an approximate algorithm it takes, over the model's
# parameters, the largest distance of the posterior mean from the NUTS mean
# in NUTS posterior standard deviations, and the smallest and largest ratio
# of the posterior sd to the NUTS sd. A model's runs are then reduced to the
# median and the worst run. Parameters whose NUTS sd exceeds 1e6 have no
# usable mean and are left out, by name.

args <- commandArgs(trailingOnly = TRUE)
d <- read.csv(args[1], stringsAsFactors = FALSE)
notes <- read.csv(sub("\\.csv$", "_notes.csv", args[1]),
                  stringsAsFactors = FALSE)
ref <- d[d$algorithm == "sampling", c("model", "variable", "mean", "sd")]
names(ref)[3:4] <- c("nuts_mean", "nuts_sd")
unusable <- ref[ref$nuts_sd > 1e6, c("model", "variable")]
if (nrow(unusable))
  cat("left out (no usable NUTS mean):",
      paste(unusable$model, unusable$variable, collapse = "; "), "\n\n")
ref <- ref[ref$nuts_sd <= 1e6, ]
a <- merge(d[d$algorithm != "sampling", ], ref, by = c("model", "variable"))
a$z <- abs(a$mean - a$nuts_mean) / a$nuts_sd
a$ratio <- a$sd / a$nuts_sd
runs <- do.call(rbind, lapply(
  split(a, list(a$model, a$backend, a$algorithm, a$seed), drop = TRUE),
  function(x) data.frame(
    model = x$model[1], backend = x$backend[1], algorithm = x$algorithm[1],
    seed = x$seed[1], parameters = nrow(x), z = max(x$z),
    low = min(x$ratio), high = max(x$ratio), distinct = x$distinct_draws[1])))
f <- function(x)
  if (x >= 1000) formatC(x, format = "e", digits = 1) else
    formatC(x, format = "f", digits = 2)
failed <- function(m, b, alg) {
  w <- notes[notes$model == m & notes$backend == b & notes$algorithm == alg, ]
  sum(grepl("^ERROR", w$warnings))
}
warned <- function(m, b, alg) {
  w <- notes[notes$model == m & notes$backend == b & notes$algorithm == alg, ]
  sum(!is.na(w$warnings) & nzchar(w$warnings) & !grepl("^ERROR", w$warnings))
}

cat("| model | parameters | algorithm | runs that finished | mean error, median run (worst) | sd ratio, median run | runs with a warning |\n")
cat("|---|---|---|---|---|---|---|\n")
for (m in unique(d$model)) {
  for (alg in c("meanfield", "fullrank", "pathfinder", "laplace")) {
    x <- runs[runs$model == m & runs$backend == "stanli" &
                runs$algorithm == alg, ]
    if (!nrow(x)) next
    cat(sprintf("| %s | %d | %s | %d of %d | %s (%s) | %s to %s | %d |\n",
                m, x$parameters[1], alg, nrow(x),
                nrow(x) + failed(m, "stanli", alg), f(median(x$z)), f(max(x$z)),
                f(median(x$low)), f(median(x$high)), warned(m, "stanli", alg)))
  }
}

cat("\nPathfinder, distinct draws among the 1000 returned, per run:\n")
p <- runs[runs$algorithm == "pathfinder", ]
for (m in unique(p$model))
  cat(sprintf("  %s: %s\n", m, paste(p$distinct[p$model == m], collapse = ", ")))

both <- merge(d[d$backend == "stanli", ], d[d$backend == "rstan", ],
              by = c("model", "algorithm", "seed", "variable"),
              suffixes = c(".stanli", ".rstan"))
if (nrow(both)) {
  both$mean_diff <- abs(both$mean.stanli - both$mean.rstan)
  both$sd_diff <- abs(both$sd.stanli - both$sd.rstan)
  cat("\nstanli against rstan::vb(), same seed: largest absolute difference",
      "of a posterior mean and of a posterior sd\n")
  cat("| model | algorithm | runs | values | mean | sd |\n|---|---|---|---|---|---|\n")
  for (m in unique(both$model)) for (alg in unique(both$algorithm)) {
    x <- both[both$model == m & both$algorithm == alg, ]
    cat(sprintf("| %s | %s | %d | %d | %.1e | %.1e |\n", m, alg,
                length(unique(x$seed)), nrow(x), max(x$mean_diff),
                max(x$sd_diff)))
  }
}

cat("\nFailures:\n")
e <- notes[grepl("^ERROR", notes$warnings), ]
for (i in seq_len(nrow(e)))
  cat(sprintf("  %s %s %s seed %d: %s\n", e$model[i], e$backend[i],
              e$algorithm[i], e$seed[i], substr(e$warnings[i], 8, 200)))
