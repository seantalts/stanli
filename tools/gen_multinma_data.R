#!/usr/bin/env Rscript
# Regenerates tests/multinma: two of multinma's Stan models with their
# #include lines inlined, and the Stan data multinma builds for two of its
# example networks.
#
# multinma (https://cran.r-project.org/package=multinma) is GPL-3; its author
# and maintainer is David M. Phillippo. This script drives the installed
# package, version 0.9.1 from CRAN:
#   Rscript -e 'remotes::install_version("multinma", "0.9.1")'
# The script checks every Stan source file it reads against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The models. Each model in inst/stan pulls in shared pieces with
# `#include /include/file.stan` lines. The corpus pairs one self-contained
# .stan file with one .json file, so each #include line is replaced by the
# full text of the file it names. The package's license header,
# inst/stan/include/license.stan (comments only), goes first, as rstantools
# puts it at the top of the generated C++. Nothing else is changed.
#
# The data. nma() builds the data list and passes it to rstan::sampling();
# a method on that generic defined here records the list and stops before
# any sampling happens. nma() sets chain_id = 1, which rstan turns into the
# data field CHAIN_ID; that field is added here with the same value. The
# data-block fields of the list are written with tools/stan_data_json.R.
#
# Usage:
#   Rscript tools/gen_multinma_data.R [outdir]     # default tests/multinma

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/multinma"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "include/license.stan" =
    "b16b48e411b01744dc5fcd13727dc4d09cafa5aa26a76ad1f465929c58235159",
  "binomial_1par.stan" =
    "ea0e7bd62576f71b76003201ac5119780452f0f15d0ec9035bdb44b680e683cb",
  "include/prior_select.stan" =
    "5ab3f613b3e1c7b234fd58782db174b708e9b879be24fd341008340210cd595b",
  "include/count_nonzero.stan" =
    "a44a9e191fbef99be4acb33fbbc65d5f2a96f547a1b4dd928b130fa5d3e5a68b",
  "include/data_common.stan" =
    "3f39102ed8817c0d65672efbc253f6246aa7f25e1544ef2c2506e2c777dfed04",
  "include/transformed_data_common.stan" =
    "5aba1e6f25ec3ffe1de6569794d002c221fb8af6a1af45c27f7ec5bfb7ba2f42",
  "include/parameters_common.stan" =
    "a794b13f301992b57686cb451e38e4276b2950059b8180d5e43837358da58e83",
  "include/transformed_parameters_theta.stan" =
    "3fe806bc6d61f9ac021f92a5116c8f07bd37e0b15925e2f23d300343e20d656a",
  "include/transformed_parameters_common.stan" =
    "73f08919c717a646b1bce475f91afe7323680f9b653da9e1b1de5f3376089d8e",
  "include/model_common.stan" =
    "1ecc18d8cdc9241b943d34c6915f13b77fb79f33775e199fd56132764adf2b4b",
  "include/generated_quantities_theta_fitted.stan" =
    "e7bb3e48d90a3cd54f6f01382078f103e1416305228302f69a08b5dac40b299d",
  "include/generated_quantities_common.stan" =
    "4cfe2a26d94c20e3c590ff5c9640efb8fb5f7d0daf5c85ee87f13ea255480a3a",
  "include/generated_quantities_theta.stan" =
    "a98b53e06ec95c819011b1ddf87674352c9e396a35de5d2b79c2bd3a4f9553e7",
  "survival_mspline.stan" =
    "f909b27f94c643ce4f96b606414805f59f84f69e978936a78d12e2c5912e467c",
  "include/vector_functions.stan" =
    "95d4ee974f8ba18468786417f49bd0b6d6530eb229fc8826a7de40ae2e22a8ea")

for (p in c("multinma", "rstan")) {
  if (!requireNamespace(p, quietly = TRUE)) stop(p, " is not installed")
}
suppressPackageStartupMessages({
  library(rstan)
  library(multinma)
})
stopifnot(packageDescription("multinma")$Version == "0.9.1")

stan_dir <- system.file("stan", package = "multinma")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed multinma has no inst/stan/", rel)
  if (is.na(SHA256[rel])) stop("no recorded hash for inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed multinma's ", rel, " does not match 0.9.1\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}

inline <- function(rel) {
  out <- character(0)
  for (line in read_checked(rel)) {
    m <- regmatches(line, regexec("^\\s*#include\\s+/?(\\S+)\\s*$", line))[[1]]
    out <- c(out, if (length(m) == 2) inline(m[2]) else line)
  }
  out
}

model_lines <- function(file) {
  lines <- c(read_checked("include/license.stan"), inline(file))
  if (any(grepl("^\\s*#include", lines))) stop(file, ": an #include is left")
  lines
}

cap <- new.env()
setMethod(rstan::sampling, "stanmodel", function(object, ...) {
  a <- list(...)
  cap$model <- object@model_name
  cap$data <- a$data
  cap$chain_id <- a$chain_id
  stop("standata captured")
}, where = globalenv())
capture <- function(expr) {
  cap$model <- NULL
  cap$data <- NULL
  res <- try(suppressWarnings(suppressMessages(expr)), silent = TRUE)
  if (is.null(cap$data)) stop("multinma did not reach rstan::sampling: ", res)
  list(model = cap$model, data = cap$data, chain_id = cap$chain_id)
}

# The ndmm network has 4,144 patients in five studies. Twenty per arm, drawn
# with a fixed seed, keep every study and arm and both data types.
ndmm_subsample <- function(d, per_arm = 20) {
  set.seed(20261114)
  keep <- unlist(lapply(split(seq_len(nrow(d)), list(d$study, d$trt), drop = TRUE),
                        function(i) i[sort(sample.int(length(i), min(per_arm, length(i))))]))
  d[sort(keep), ]
}

examples <- list(
  multinma_smoking_re = list(file = "binomial_1par.stan", run = function() {
    smk_net <- set_agd_arm(smoking, study = studyn, trt = trtc, r = r, n = n,
                           trt_ref = "No intervention")
    nma(smk_net, trt_effects = "random",
        prior_intercept = normal(scale = 100),
        prior_trt = normal(scale = 100),
        prior_het = normal(scale = 5))
  }),
  multinma_ndmm_mspline = list(file = "survival_mspline.stan", run = function() {
    ndmm_net <- combine_network(
      set_ipd(ndmm_subsample(ndmm_ipd), study, trt,
              Surv = Surv(eventtime / 12, status)),
      set_agd_surv(ndmm_subsample(ndmm_agd), study, trt,
                   Surv = Surv(eventtime / 12, status)))
    nma(ndmm_net, likelihood = "mspline",
        prior_intercept = normal(0, 100),
        prior_trt = normal(0, 100),
        prior_aux = half_normal(1))
  }))

for (id in names(examples)) {
  ex <- examples[[id]]
  code_lines <- model_lines(ex$file)
  code <- paste(code_lines, collapse = "\n")
  data_fields <- names(decl_ndim(code))
  got <- capture(ex$run())
  if (got$model != sub("\\.stan$", "", ex$file)) {
    stop(id, ": multinma chose ", got$model, ", expected ", ex$file)
  }
  sd <- got$data
  if (is.null(sd$CHAIN_ID)) sd$CHAIN_ID <- as.integer(got$chain_id)
  missing_fields <- setdiff(data_fields, names(sd))
  if (length(missing_fields) > 0) {
    stop(id, ": standata is missing data-block fields: ",
         paste(missing_fields, collapse = ", "))
  }
  sd <- sd[data_fields]
  # Arrays of vectors such as prior_aux_location come as lists of vectors,
  # which write_json skips; one row per array element gives the same JSON.
  for (k in data_fields) if (is.list(sd[[k]])) sd[[k]] <- do.call(rbind, sd[[k]])
  for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
  # transformed data takes the CSR branch when at most 10% of the Cholesky
  # factor of RE_cor is nonzero.
  n_delta <- if (sd$RE) max(sd$which_RE) else 0
  sparse <- if (n_delta > 0) sum(chol(sd$RE_cor) != 0) / length(sd$RE_cor) else NA
  writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, code, file.path(OUT, paste0(id, ".json")))
  cat(sprintf("%-24s ni_ipd=%d ni_agd_arm=%d nt=%d RE=%d n_delta=%d nonzero=%s\n",
              id, as.integer(sd$ni_ipd), as.integer(sd$ni_agd_arm),
              as.integer(sd$nt), as.integer(sd$RE), as.integer(n_delta),
              if (is.na(sparse)) "-" else sprintf("%.3f", sparse)))
}
