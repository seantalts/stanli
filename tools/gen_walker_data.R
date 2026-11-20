#!/usr/bin/env Rscript
# Regenerates tests/walker: two of walker's Stan models, rw1_model.stan (a
# Kalman filter for a dynamic regression with random walk coefficients) and
# rw1_model_naive.stan (the same model with the coefficients as parameters),
# with data built the way walker_rw1() builds it for its own example.
#
# walker (https://cran.r-project.org/package=walker) is GPL (>= 3), version
# 1.0.10. This script reads the unpacked CRAN source tarball, so nothing needs
# to be installed or compiled:
#   curl -O https://cran.r-project.org/src/contrib/walker_1.0.10.tar.gz
#   tar -xzf walker_1.0.10.tar.gz          # into an empty directory
#   Rscript tools/gen_walker_data.R path/to/walker [outdir]
#
# The models have no #include lines, so each .stan file is a verbatim copy of
# inst/stan/<model>.stan.
#
# The data. The package has no data set. The simulation is the first example
# of walker_rw1() in R/walker_rw1.R (set.seed(123), n = 100, two random walk
# coefficients and an independent random walk level u), and the Stan data list
# is the one walker_rw1() makes from `y ~ x1 + x2` with
# beta = cbind(0, rep(2, 3)) and sigma = cbind(0, rep(2, 4)): an intercept
# column and the two covariates, k = 3, and no new observations (n_new = 0).
# Both models read the same list, restricted to their data blocks.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) stop("usage: gen_walker_data.R PACKAGE_SOURCE_DIR [OUTDIR]")
PKG <- args[1]
OUT <- if (length(args) >= 2 && nzchar(args[2])) args[2] else "tests/walker"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))
stopifnot(read.dcf(file.path(PKG, "DESCRIPTION"), "Version")[1] == "1.0.10")

set.seed(123)
n <- 100
beta1 <- cumsum(c(0.5, rnorm(n - 1, 0, sd = 0.05)))
beta2 <- cumsum(c(-1, rnorm(n - 1, 0, sd = 0.15)))
x1 <- rnorm(n, 1)
x2 <- 0.25 * cos(1:n)
u <- cumsum(rnorm(n))
y <- rnorm(n, u + beta1 * x1 + beta2 * x2)

xreg <- model.matrix(y ~ x1 + x2)
k <- ncol(xreg)
beta <- cbind(0, rep(2, 3))
sigma <- cbind(0, rep(2, 4))
stan_data <- list(k = k, n = n, y = y, xreg = t(xreg),
                  n_new = 0L, xreg_new = lapply(seq_len(k), function(i) numeric(0)),
                  beta_mean = structure(beta[, 1], dim = k),
                  beta_sd = structure(beta[, 2], dim = k),
                  sigma_mean = sigma[, 1], sigma_sd = sigma[, 2])

for (name in c("rw1_model", "rw1_model_naive")) {
  src <- file.path(PKG, "inst", "stan", paste0(name, ".stan"))
  stan <- file.path(OUT, paste0("walker_", name, ".stan"))
  file.copy(src, stan, overwrite = TRUE)
  write_json(stan_data, paste(readLines(stan), collapse = "\n"),
             file.path(OUT, paste0("walker_", name, ".json")))
  cat(name, "\n")
}
