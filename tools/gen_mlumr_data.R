#!/usr/bin/env Rscript
# Regenerates tests/mlumr: two of mlumr's survival models with their
# #includes inlined, and the Stan data the package builds for a subsample of
# its own NDMM survival vignette example.
#
# mlumr (https://cran.r-project.org/package=mlumr) is GPL-3, by Ahmad
# Sofi-Mahmudi and Conor Chandler. This script drives the installed
# package, version 0.2.0 from CRAN:
#   Rscript -e 'remotes::install_version("mlumr", "0.2.0")'
# It checks the installed package's Stan sources against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The models. Each checked-in model is the package's model file with every
# `#include` line replaced by the text of the file it names. Nothing else is
# changed.
#
# The data. The survival vignette compares lenalidomide (McCarthy2012, 231
# patients of individual data) with thalidomide (Morgan2012, 408 rows of
# pseudo-data reconstructed from a Kaplan-Meier curve) over 64 integration
# points; a full fit there takes about 20 minutes per chain. Here every fifth
# row of each arm is kept and add_integration() uses 16 points. The data then
# goes through the package's own set_ipd(), set_agd_surv(), combine_data(),
# add_integration() and mlumr(). mlumr() hands the finished data list to the
# internal fit_rstan(); that function is replaced in the namespace by one that
# records the model name and data and stops, so no sampling runs.
#
# Usage:
#   Rscript tools/gen_mlumr_data.R [outdir]     # default tests/mlumr

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/mlumr"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "mlumr_survival_relaxed.stan" =
    "dc54571d56e1af52fc440afe55deb3b75a9873a763fcad0819a26db4c7c15793",
  "mlumr_survival_mspline_relaxed.stan" =
    "d655dee850a5b7e1c2a1fb35492816d7513aeebff6697be903ca1b67e97b2d1c",
  "include/priors_functions.stan" =
    "c3a1a69d60bd61d0dc7399ec823bca0656d44d88bd8f91c25778cd82871d3196",
  "include/survival_functions.stan" =
    "e7086744a4fadff7de43baae0720df1976c0a7dd667741e93fabccc9372ea5ce",
  "include/survival_mspline_functions.stan" =
    "bb4a1eedd5e3b5238df2e36f1e755eff54169e5e1b6c6694cf0781fc01adcac8",
  "include/priors_hyperparameters.stan" =
    "eccb78dca1fcdfe4c49e4cc00195899cf37a5c498fbb0a3ac228d5361ebf0a13",
  "include/survival_aux_hyperparameters.stan" =
    "2c05efc7ab074444ed3ceacdda7404900d17370023da73ff2fc6afff6a70771a")

if (!requireNamespace("mlumr", quietly = TRUE)) {
  stop("mlumr is not installed; install version 0.2.0 from CRAN.")
}
suppressPackageStartupMessages(library(mlumr))
stopifnot(packageDescription("mlumr")$Version == "0.2.0")

stan_dir <- system.file("stan", package = "mlumr")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed mlumr has no inst/stan/", rel)
  if (is.na(SHA256[rel])) stop("no recorded hash for inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed mlumr's ", rel, " does not match 0.2.0\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}

flatten <- function(model_file) {
  out <- character(0)
  for (l in read_checked(model_file)) {
    if (grepl("^\\s*#include", l)) {
      rel <- sub("^/", "", trimws(sub("^\\s*#include", "", l)))
      inc <- read_checked(rel)
      if (any(grepl("^\\s*#include", inc))) stop(rel, " has nested includes")
      out <- c(out, inc)
    } else {
      out <- c(out, l)
    }
  }
  out
}

capture_stan_data <- function(expr) {
  captured <- NULL
  original <- get("fit_rstan", envir = asNamespace("mlumr"))
  assignInNamespace("fit_rstan", function(model_name, stan_data, ...) {
    captured <<- list(model_name = model_name, stan_data = stan_data)
    stop("stan data captured")
  }, ns = "mlumr")
  on.exit(assignInNamespace("fit_rstan", original, ns = "mlumr"))
  tryCatch(expr, error = function(e) {
    if (conditionMessage(e) != "stan data captured") stop(e)
  })
  if (is.null(captured)) stop("fit_rstan was not reached")
  captured
}

data("ndmm_ipd", package = "mlumr", envir = environment())
data("ndmm_agd", package = "mlumr", envir = environment())
data("ndmm_agd_covs", package = "mlumr", envir = environment())
covs <- c("age", "iss_stage3", "response_cr_vgpr", "male")
ipd_one <- ndmm_ipd[ndmm_ipd$study == "McCarthy2012" &
                    ndmm_ipd$treatment == "Len", ]
agd_one <- ndmm_agd[ndmm_agd$study == "Morgan2012" &
                    ndmm_agd$treatment == "Thal", ]
ipd_one <- ipd_one[seq(1, nrow(ipd_one), by = 5), ]
agd_one <- agd_one[seq(1, nrow(agd_one), by = 5), ]
cv <- ndmm_agd_covs[ndmm_agd_covs$study == "Morgan2012" &
                    ndmm_agd_covs$treatment == "Thal", ]
agd_one$age_mean <- cv$age_mean
agd_one$age_sd <- cv$age_sd
agd_one$iss_stage3_prop <- cv$iss_stage3_prop
agd_one$response_cr_vgpr_prop <- cv$response_cr_vgpr_prop
agd_one$male_prop <- cv$male_prop
tau_rmst <- min(max(ipd_one$eventtime), max(agd_one$eventtime))

ipd <- set_ipd(ipd_one, treatment = "treatment", covariates = covs,
               family = "survival", time = "eventtime", status = "status")
agd <- set_agd_surv(agd_one, treatment = "treatment",
                    time = "eventtime", status = "status",
                    cov_means = c("age_mean", "iss_stage3_prop",
                                  "response_cr_vgpr_prop", "male_prop"),
                    cov_sds = c("age_sd", NA, NA, NA),
                    cov_types = c("continuous", "binary", "binary", "binary"))
dat <- suppressWarnings(combine_data(ipd, agd))
set.seed(20261114)
dat <- suppressWarnings(add_integration(
  dat, n_int = 16,
  age = distr(qgamma, mean = age_mean, sd = age_sd),
  iss_stage3 = distr(qbern, prob = iss_stage3_mean),
  response_cr_vgpr = distr(qbern, prob = response_cr_vgpr_mean),
  male = distr(qbern, prob = male_mean)))

entries <- list(
  mlumr_survival_relaxed = list(distribution = "weibull"),
  mlumr_survival_mspline_relaxed = list(distribution = "mspline", n_knots = 7))

for (id in names(entries)) {
  got <- capture_stan_data(suppressWarnings(suppressMessages(
    do.call(mlumr, c(list(dat, model = "relaxed", rmst_horizon = tau_rmst,
                          prior_beta_comparator =
                            prior_normal(0, 1, autoscale = TRUE),
                          engine = "rstan", seed = 2026, verbose = FALSE),
                     entries[[id]])))))
  if (got$model_name != id) {
    stop(id, ": mlumr chose Stan model ", got$model_name)
  }
  code_lines <- flatten(paste0(id, ".stan"))
  code <- paste(code_lines, collapse = "\n")
  data_fields <- names(decl_ndim(code))
  sd <- got$stan_data
  missing_fields <- setdiff(data_fields, names(sd))
  if (length(missing_fields) > 0) {
    stop(id, ": stan data is missing data-block fields: ",
         paste(missing_fields, collapse = ", "))
  }
  sd <- sd[data_fields]
  for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
  writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, code, file.path(OUT, paste0(id, ".json")))
  cat(sprintf("%-32s n_ipd=%d n_agd=%d n_int=%d n_cov=%d n_strata=%d%s\n",
              id, sd$n_ipd, sd$n_agd, sd$n_int, sd$n_cov, sd$n_strata,
              if (!is.null(sd$dist)) paste0(" dist=", sd$dist)
              else paste0(" n_scoef=", sd$n_scoef)))
}
