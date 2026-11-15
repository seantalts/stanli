#!/usr/bin/env Rscript
# Regenerates tests/beeguts: BeeGUTS's GUTS_SD.stan with its #includes
# inlined, and the Stan data fitBeeGUTS() builds for the package's
# betacyfluthrinChronic example.
#
# BeeGUTS (https://cran.r-project.org/package=BeeGUTS) is GPL-3, copyright
# Bayer AG and ibacon GmbH. This script drives the installed
# package, version 1.5.0 from CRAN:
#   Rscript -e 'remotes::install_version("BeeGUTS", "1.5.0")'
# It checks the installed package's Stan sources against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The model. The checked-in model is GUTS_SD.stan with each of its four
# `#include` lines replaced by the text of the file it names. Nothing else is
# changed.
#
# The data. fitBeeGUTS(modelType = "SD") with the default rk45 integrator
# builds the data list and passes it to rstan::sampling() with the GUTS_SD
# model. rstan::sampling is replaced in the rstan namespace by a function that
# records the model name and data and stops, so no sampling runs.
#
# Usage:
#   Rscript tools/gen_beeguts_data.R [outdir]     # default tests/beeguts

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/beeguts"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "GUTS_SD.stan" =
    "38db5557bdfa456536e5071fd6ea11e97297b6e7dcf568b88f3ac12542a28d58",
  "include/common_stan_functions.stan" =
    "97ff193cbfb114168e483dd22a13a083d72c44b41e320d6470fb71fc6ed3b3ae",
  "include/data_guts.stan" =
    "038e74ffb208e7bc2e9555d76f68a5431b9eaf9d4f88025f310f968356756e1c",
  "include/gen_quantities_guts.stan" =
    "4bc065156f5721c890acc4f0048f2fda04ee7946d8ce7f143b5d1e234e90d192",
  "include/license.stan" =
    "df8f5493dc632e0ab0d330277abb171caa5e19c6eb8d11a476243018adfd1809")

if (!requireNamespace("BeeGUTS", quietly = TRUE)) {
  stop("BeeGUTS is not installed; install version 1.5.0 from CRAN.")
}
suppressPackageStartupMessages(library(BeeGUTS))
stopifnot(packageDescription("BeeGUTS")$Version == "1.5.0")

stan_dir <- system.file("stan", package = "BeeGUTS")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed BeeGUTS has no inst/stan/", rel)
  if (is.na(SHA256[rel])) stop("no recorded hash for inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed BeeGUTS's ", rel, " does not match 1.5.0\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}

code_lines <- character(0)
for (l in read_checked("GUTS_SD.stan")) {
  if (grepl("^\\s*#include", l)) {
    rel <- sub("^/", "", trimws(sub("^\\s*#include", "", l)))
    inc <- read_checked(rel)
    if (any(grepl("^\\s*#include", inc))) stop(rel, " has nested includes")
    code_lines <- c(code_lines, inc)
  } else {
    code_lines <- c(code_lines, l)
  }
}
code <- paste(code_lines, collapse = "\n")

captured <- NULL
original <- get("sampling", envir = asNamespace("rstan"))
assignInNamespace("sampling", function(object, data, ...) {
  captured <<- list(model_name = object@model_name, data = data)
  stop("stan data captured")
}, ns = "rstan")
data("betacyfluthrinChronic", package = "BeeGUTS", envir = environment())
tryCatch(
  suppressWarnings(fitBeeGUTS(betacyfluthrinChronic, modelType = "SD",
                              parallel = FALSE)),
  error = function(e) {
    if (conditionMessage(e) != "stan data captured") stop(e)
  })
assignInNamespace("sampling", original, ns = "rstan")
if (is.null(captured)) stop("rstan::sampling was not reached")
if (captured$model_name != "GUTS_SD") {
  stop("fitBeeGUTS chose Stan model ", captured$model_name)
}

id <- "beeguts_guts_sd"
sd <- captured$data
data_fields <- names(decl_ndim(code))
missing_fields <- setdiff(data_fields, names(sd))
if (length(missing_fields) > 0) {
  stop(id, ": stan data is missing data-block fields: ",
       paste(missing_fields, collapse = ", "))
}
sd <- sd[data_fields]
for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
write_json(sd, code, file.path(OUT, paste0(id, ".json")))
cat(sprintf("%-16s nDatasets=%d nGroup=%d nData_Nsurv=%d nData_conc=%d\n", id,
            sd$nDatasets, sd$nGroup, sd$nData_Nsurv, sd$nData_conc))
