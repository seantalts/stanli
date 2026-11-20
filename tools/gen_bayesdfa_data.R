#!/usr/bin/env Rscript
# Regenerates tests/bayesdfa: bayesdfa's dynamic factor analysis model,
# dfa.stan, with the Stan data that the package's own fit_dfa() builds for two
# of its examples.
#
# bayesdfa (https://cran.r-project.org/package=bayesdfa) is GPL (>= 3),
# version 1.3.4. This script reads the unpacked CRAN source tarball, so
# nothing needs to be installed or compiled:
#   curl -O https://cran.r-project.org/src/contrib/bayesdfa_1.3.4.tar.gz
#   tar -xzf bayesdfa_1.3.4.tar.gz         # into an empty directory
#   Rscript tools/gen_bayesdfa_data.R path/to/bayesdfa [outdir]
# It needs dplyr, reshape2, mgcv and loo, which the package imports.
#
# The model has no #include lines, so each .stan file is a verbatim copy of
# inst/stan/dfa.stan. The corpus pairs one .stan file with one .json file, so
# the two entries have a copy each.
#
# The data. The script sources the package's R files except the ones that load
# the compiled models, and calls fit_dfa(..., estimation = "none"), which
# returns the argument list that would go to rstan::sampling() before any
# sampling. Its `data` element is the Stan data. The two calls are the
# package's own examples, on the data of
# `set.seed(42); sim_dfa(num_trends = 1, num_years = 20, num_ts = 3)`:
#   dfa              fit_dfa(y = s$y_sim)
#   dfa_gp           fit_dfa(y = s$y_sim, trend_model = "gp", n_knots = 5)

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) stop("usage: gen_bayesdfa_data.R PACKAGE_SOURCE_DIR [OUTDIR]")
PKG <- args[1]
OUT <- if (length(args) >= 2 && nzchar(args[2])) args[2] else "tests/bayesdfa"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))
stopifnot(read.dcf(file.path(PKG, "DESCRIPTION"), "Version")[1] == "1.3.4")

stanmodels <- list(dfa = NULL)
skip <- c("stanmodels.R", "zzz.R", "bayesdfa-package.R")
for (f in setdiff(list.files(file.path(PKG, "R"), pattern = "[.]R$"), skip)) {
  source(file.path(PKG, "R", f), local = FALSE)
}

src <- file.path(PKG, "inst", "stan", "dfa.stan")
code <- paste(readLines(src), collapse = "\n")

set.seed(42)
s <- sim_dfa(num_trends = 1, num_years = 20, num_ts = 3)

CASES <- list(
  dfa = list(),
  dfa_gp = list(trend_model = "gp", n_knots = 5))
for (name in names(CASES)) {
  file.copy(src, file.path(OUT, paste0("bayesdfa_", name, ".stan")), overwrite = TRUE)
  fit <- do.call(fit_dfa, c(list(y = s$y_sim, estimation = "none"), CASES[[name]]))
  write_json(fit$sampling_args$data, code,
             file.path(OUT, paste0("bayesdfa_", name, ".json")))
  cat(name, "\n")
}
