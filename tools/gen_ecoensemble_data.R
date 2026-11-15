#!/usr/bin/env Rscript
# Regenerates tests/ecoensemble: EcoEnsemble's
# ensemble_model_hierarchical_withdrivers.stan with the Stan data the
# package builds for its own drivers test data set, under two prior
# configurations.
#
# EcoEnsemble (https://cran.r-project.org/package=EcoEnsemble) is GPL (>= 3),
# by Michael A. Spence, James A. Martindale, Michael J. Thomson, Thomas I. J.
# Bartos and Luke E. K. Broadbent. This script drives the installed package
# (installed separately by whoever runs this), which must be 1.2.0.
#
# The model. At install time rstantools::rstan_config() rewrites every file
# in inst/stan/ into stanc's canonical format, so the installed copy depends
# on the local rstan version. The checked-in model is the file as published
# in the CRAN tarball instead, read from the tarball given as the second
# argument (downloaded from CRAN when it is omitted). The script checks the
# tarball's and the file's SHA-256 before writing anything. The file has no
# #include lines and is copied verbatim, twice, because the corpus inventory
# pairs one .stan file with one .json file.
#
# The data. The package ships no example data with drivers (its North Sea
# data sets have no driver dimension). Its testthat file
# tests/testthat/test-post_process_withdrivers.R defines a small one: 2
# species, 2 simulators by 3 drivers, 10 years of model output and 8 years
# of observations. Those numbers are copied below. EnsembleData(...,
# drivers = TRUE) turns them into the list that fit_ensemble_model() passes
# to rstan, available as its @stan_input slot, so nothing is sampled.
#
# Usage:
#   Rscript tools/gen_ecoensemble_data.R [outdir] [EcoEnsemble_1.2.0.tar.gz]

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/ecoensemble"
TARBALL <- if (length(args) >= 2 && nzchar(args[2])) args[2] else ""
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

TARBALL_SHA256 <-
  "769b611f50055b169864df8038f67a71973da99e1160cae32539b0f3db4b37e2"
MODEL <- "EcoEnsemble/inst/stan/ensemble_model_hierarchical_withdrivers.stan"
MODEL_SHA256 <-
  "a5e83a3989c04909d255cbb71f4a06297dfebe3523702d99a9e7139a08e33c58"

if (!requireNamespace("EcoEnsemble", quietly = TRUE)) {
  stop("EcoEnsemble is not installed; install version 1.2.0 from CRAN.")
}
stopifnot(packageDescription("EcoEnsemble")$Version == "1.2.0")
suppressPackageStartupMessages(library(EcoEnsemble))

sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
sha256 <- function(path) {
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
}
check_sha <- function(path, expected, what) {
  actual <- sha256(path)
  if (actual != expected) {
    stop(what, " does not match EcoEnsemble 1.2.0\n",
         "  expected sha256 ", expected, "\n  actual   sha256 ", actual)
  }
}

if (!nzchar(TARBALL)) {
  TARBALL <- tempfile(fileext = ".tar.gz")
  urls <- c(
    "https://cran.r-project.org/src/contrib/EcoEnsemble_1.2.0.tar.gz",
    "https://cran.r-project.org/src/contrib/Archive/EcoEnsemble/EcoEnsemble_1.2.0.tar.gz")
  ok <- FALSE
  for (u in urls) {
    ok <- tryCatch(utils::download.file(u, TARBALL, mode = "wb",
                                        quiet = TRUE) == 0,
                   error = function(e) FALSE, warning = function(w) FALSE)
    if (ok) break
  }
  if (!ok) stop("could not download EcoEnsemble_1.2.0.tar.gz from CRAN")
}
check_sha(TARBALL, TARBALL_SHA256, TARBALL)
exdir <- tempfile()
utils::untar(TARBALL, files = MODEL, exdir = exdir)
model_path <- file.path(exdir, MODEL)
check_sha(model_path, MODEL_SHA256, MODEL)
model <- readLines(model_path, warn = FALSE)
if (any(grepl("^\\s*#include", model))) stop("unexpected #include in ", MODEL)
code <- paste(model, collapse = "\n")
# decl_ndim() reads "=" as the start of an initializer, so the "==" inside
# sizes such as vector[form_prior_ind_st == 3 ? 4 : 3] would cut the
# declaration short. The shapes are read from a copy with "==" masked.
shape_code <- gsub("==", "~~", code, fixed = TRUE)

# The drivers data set of tests/testthat/test-post_process_withdrivers.R.
d <- 2
M <- 2
MM <- 3
Time <- 10
Times_obs <- round(Time * 0.8)
obs <- matrix(c(0.57,0.75,0.9,1,1.07,1.09,1.04,0.94,0.99,0.54,0.57,1.09,1.67,2.01,2.17,2.33),ncol=d)
obs.cov <- matrix(c(0.07,-0.02,-0.02,0.01),ncol=d)
model.cov <- array(0,dim=c(M * MM,d,d))
models_output <- array(NA,dim=c(M * MM,Time,d))
models_output[1,,] <- c(-0.14,0.3,0.78,1.19,1.38,1.27,1.04,0.97,1.27,1.85,3.75,3.18,3.01,3.33,3.91,4.41,4.71,4.97,5.42,6.08)
models_output[2,,] <- c(-0.94,-0.55,-0.25,-0.15,-0.26,-0.49,-0.7,-0.73,-0.52,-0.17,1.48,0.93,0.72,0.98,1.56,2.14,2.54,2.78,2.99,3.25)
models_output[3,,] <- c(-0.05, 0.35, 0.84, 1.15, 1.18, 1.17, 1.08, 1.16, 1.94, 2.39, 3.93, 3.19, 2.88, 3.17, 3.69, 3.89, 3.99, 4.85, 5.6, 6.2)
models_output[4,,] <- c(-1.16,-0.75,-0.65,-0.56,-0.66,-0.79,-0.9,-0.43,-0.02, 0.17,1.18,0.27,0.12,0.38,0.46,0.54,0.94,2.08,2.71,3.35)
models_output[5,,] <- c(-1.14, 1.3, 1.78, 2.19, 2.38, 2.27, 2.04, 1.97, 2.27, 2.85, 4.75, 4.18, 4.01, 4.33, 4.91, 5.41, 5.71, 5.97, 6.42, 7.08)
models_output[6,,] <- c(0.06, 0.45, 0.75, 0.85, 0.74, 0.51, 0.3, 0.27, 0.48, 0.83, 2.84, 1.93, 1.72, 1.98, 2.56, 3.14, 3.54, 3.78, 3.99, 4.25)
model.cov[1,,] <- c(0.06,0,0,0.03)
model.cov[2,,] <- c(0.04,0.01,0.01,0.02)
model.cov[3,,] <- c(0.05, 0, 0, 0.02)
model.cov[4,,] <- c(0.05, -0.03, -0.03, 0.06)
model.cov[5,,] <- c(0.07,-0.01, -0.01, 0.08)
model.cov[6,,] <- c(0.06, 0.03, 0.03, 0.05)

SPECIES_NAMES <- c("Species 1", "Species 2")
val_obs <- data.frame(obs)
dimnames(val_obs) <- list(paste(1:Times_obs), SPECIES_NAMES)
cov_obs <- obs.cov
dimnames(cov_obs) <- list(SPECIES_NAMES, SPECIES_NAMES)
# models_output[k, , ] is simulator (k - 1) %% 2 + 1 with driver
# (k - 1) %/% 2 + 1, the order the test file uses.
sim <- function(i) {
  ks <- i + 2 * (0:(MM - 1))
  vals <- lapply(ks, function(k) {
    v <- data.frame(models_output[k, , ])
    dimnames(v) <- list(paste(1:Time), SPECIES_NAMES)
    v
  })
  covs <- lapply(ks, function(k) {
    v <- model.cov[k, , ]
    dimnames(v) <- list(SPECIES_NAMES, SPECIES_NAMES)
    v
  })
  list(vals, covs, paste("Simulator", i), paste("Driver", 1:MM))
}
simulators <- list(sim(1), sim(2))

priors <- list(
  # EnsemblePrior's defaults: hierarchical individual short-term
  # correlations (form 3), LKJ long-term and shared short-term correlations.
  ecoensemble_hierarchical_withdrivers = EnsemblePrior(d),
  # The conjugate hierarchical beta prior (form 4) is the only path to
  # lbeta(); the inverse Wishart and beta correlation priors are those of
  # priors2 in the same test file.
  ecoensemble_hierarchical_withdrivers_conj = EnsemblePrior(d,
    ind_st_params = IndSTPrior("hierarchical_beta_conjugate",
                               list(1, 2, 10, 1), list(0.75, 0.75, 0.2)),
    ind_lt_params = IndLTPrior("inv_wishart", list(c(1, 2), c(1, 1)),
                               list(10, diag(2))),
    sha_st_params = ShaSTPrior("beta", list(c(1, 2), c(1, 1)),
                               list(matrix(5, 2, 2), matrix(2, 2, 2)))))

fields <- names(decl_ndim(shape_code))
for (id in names(priors)) {
  ens <- EnsembleData(observations = list(val_obs, cov_obs),
                      simulators = simulators, priors = priors[[id]],
                      drivers = TRUE)
  sd <- ens@stan_input
  missing_fields <- setdiff(fields, names(sd))
  if (length(missing_fields) > 0) {
    stop(id, ": stan_input is missing data-block fields: ",
         paste(missing_fields, collapse = ", "))
  }
  sd <- sd[fields]
  writeLines(model, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, shape_code, file.path(OUT, paste0(id, ".json")))
  cat(sprintf("%-42s N=%d time=%d M=%d MM=%d forms ind_st=%d ind_lt=%d sha_st=%d\n",
              id, sd$N, sd$time, sd$M, sd$MM, sd$form_prior_ind_st,
              sd$form_prior_ind_lt, sd$form_prior_sha_st))
}
