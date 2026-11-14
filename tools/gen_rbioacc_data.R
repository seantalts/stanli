#!/usr/bin/env Rscript
# Regenerates tests/rbioacc: rbioacc's odeTK.stan with its #include inlined,
# and the Stan data for the package's Sialis lutaria example.
#
# rbioacc (https://cran.r-project.org/package=rbioacc) is MIT licensed,
# Copyright (c) 2021 UMR5558 lbbe MEPS Aurélie Siberchicot. This script
# drives the installed package, version 1.3.1 from CRAN:
#   Rscript -e 'remotes::install_version("rbioacc", "1.3.1")'
# It checks the installed package's Stan sources against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The model. odeTK.stan has one active include,
# `#include /include/linear_interpolation.stan`. The checked-in model is the
# package's inst/stan/include/license.stan header (comments only; rstantools
# puts it at the top of the generated C++), then odeTK.stan with that line
# replaced by the text of linear_interpolation.stan. A second include line in
# odeTK.stan is commented out and stays as it is. Nothing else is changed.
#
# The data. fitTK() on a "stanTKdataVAR" object passes it unchanged to
# rstan::sampling() with the odeTK model, and modelData_ode() is the function
# that builds that object. The data below is the call in the package's own
# tests/testthat/test-fitODE.R.
#
# Usage:
#   Rscript tools/gen_rbioacc_data.R [outdir]     # default tests/rbioacc

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/rbioacc"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "odeTK.stan" =
    "aaf4b97de57c497347a3f1a8388f8d69f33f38d7f3935930ff9a9c8d1d643a12",
  "include/linear_interpolation.stan" =
    "df05d55278c9d557bb4b9b536bfc5fba8a7fadadd6b9ccbe082aaf11454eec40",
  "include/license.stan" =
    "404da553b5c33e9948bc6b9d7af7fc6fdb5c78847b3472a500d1766c29ff67f2")

if (!requireNamespace("rbioacc", quietly = TRUE)) {
  stop("rbioacc is not installed; install version 1.3.1 from CRAN.")
}
suppressPackageStartupMessages(library(rbioacc))
stopifnot(packageDescription("rbioacc")$Version == "1.3.1")

stan_dir <- system.file("stan", package = "rbioacc")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed rbioacc has no inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed rbioacc's ", rel, " does not match 1.3.1\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}
model <- read_checked("odeTK.stan")
interp <- read_checked("include/linear_interpolation.stan")
license <- read_checked("include/license.stan")

inc <- grep("^#include", model)
if (length(inc) != 1 ||
    trimws(model[inc]) != "#include /include/linear_interpolation.stan") {
  stop("expected exactly one #include of /include/linear_interpolation.stan")
}
code_lines <- c(license, model[seq_len(inc - 1)], interp, model[-seq_len(inc)])
code <- paste(code_lines, collapse = "\n")

data("Exposure_Sialis_lutaria", package = "rbioacc", envir = environment())
data("Internal_Sialis_lutaria", package = "rbioacc", envir = environment())
Exposure_Sialis_lutaria$value <- Exposure_Sialis_lutaria$Cwater
Internal_Sialis_lutaria$value <- Internal_Sialis_lutaria$Cinternal
sd <- modelData_ode(Exposure_Sialis_lutaria, Internal_Sialis_lutaria,
                    time_accumulation = 2.170)
stopifnot(inherits(sd, "stanTKdataVAR"))

id <- "rbioacc_odetk"
data_fields <- names(decl_ndim(code))
missing_fields <- setdiff(data_fields, names(sd))
if (length(missing_fields) > 0) {
  stop(id, ": stan data is missing data-block fields: ",
       paste(missing_fields, collapse = ", "))
}
sd <- unclass(sd)[data_fields]
# Cmet is array[lentp, n_met, n_rep] with n_met = 0. Fields with no elements
# are written as a bare [], which CmdStan and stanli accept for any declared
# shape with a zero extent.
for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
write_json(sd, code, file.path(OUT, paste0(id, ".json")))
cat(sprintf("%-16s lentp=%d lentp_rmNA=%d n_met=%d rankacc=%d\n", id,
            sd$lentp, sd$lentp_rmNA, sd$n_met, sd$rankacc))
