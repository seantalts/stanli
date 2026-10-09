# Compares two runs of harnesses/brms_end_to_end.R variable by variable.
#
#   Rscript harnesses/brms_compare_backends.R A.tsv B.tsv OUT.tsv
#
# For every model both backends fitted, each variable's posterior means are
# compared in units of their combined Monte Carlo standard error:
#   z = |mean_a - mean_b| / sqrt(mcse_a^2 + mcse_b^2)
# Two correct samplers give |z| below about 4 on nearly every variable. That
# reading only holds where both runs converged, so the worst rhat of each run
# is reported beside it and unconverged models are listed apart.

args <- commandArgs(trailingOnly = TRUE)
read_run <- function(path) list(
  status = utils::read.delim(path, quote = "", stringsAsFactors = FALSE),
  summaries = readRDS(sub("\\.tsv$", ".summaries.rds", path)))
a <- read_run(args[1]); b <- read_run(args[2])
slugs <- intersect(names(a$summaries), names(b$summaries))
rows <- lapply(slugs, function(slug) {
  x <- a$summaries[[slug]]; y <- b$summaries[[slug]]
  shared <- setdiff(intersect(x$variable, y$variable), c("lp__", "lprior"))
  x <- x[match(shared, x$variable), ]; y <- y[match(shared, y$variable), ]
  se <- sqrt(x$mcse_mean^2 + y$mcse_mean^2)
  z <- abs(x$mean - y$mean) / se
  z[!is.finite(z)] <- NA
  worst <- if (all(is.na(z))) NA_integer_ else which.max(z)
  data.frame(
    slug = slug, variables = length(shared),
    only_a = length(setdiff(x$variable, shared)) + sum(!a$summaries[[slug]]$variable %in% c(shared, "lp__", "lprior")),
    only_b = sum(!b$summaries[[slug]]$variable %in% c(shared, "lp__", "lprior")),
    max_z = if (is.na(worst)) NA_real_ else z[worst],
    worst_variable = if (is.na(worst)) NA_character_ else shared[worst],
    share_z_above_3 = mean(z > 3, na.rm = TRUE),
    max_rhat_a = suppressWarnings(max(x$rhat, na.rm = TRUE)),
    max_rhat_b = suppressWarnings(max(y$rhat, na.rm = TRUE)),
    min_ess_a = suppressWarnings(min(x$ess_bulk, na.rm = TRUE)),
    min_ess_b = suppressWarnings(min(y$ess_bulk, na.rm = TRUE)),
    stringsAsFactors = FALSE)
})
out <- do.call(rbind, rows)
utils::write.table(out, args[3], sep = "\t", quote = FALSE, row.names = FALSE)
converged <- out$max_rhat_a < 1.01 & out$max_rhat_b < 1.01
cat(sprintf("%d models fitted by both; %d converged in both (rhat < 1.01)\n",
            nrow(out), sum(converged, na.rm = TRUE)))
cat(sprintf("converged models: median of max z %.2f, largest %.2f (%s)\n",
            stats::median(out$max_z[converged], na.rm = TRUE),
            max(out$max_z[converged], na.rm = TRUE),
            out$slug[converged][which.max(out$max_z[converged])]))
