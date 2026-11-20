#!/usr/bin/env Rscript
# Regenerates tests/hbayesdm: four of hBayesDM's Stan models, with the Stan
# data that the package's own R code builds from its bundled example data.
#
# hBayesDM (https://cran.r-project.org/package=hBayesDM) is GPL-3, version
# 2.0.1. This script reads the unpacked CRAN source tarball, so nothing needs
# to be installed or compiled:
#   curl -O https://cran.r-project.org/src/contrib/hBayesDM_2.0.1.tar.gz
#   tar -xzf hBayesDM_2.0.1.tar.gz        # into an empty directory
#   Rscript tools/gen_hbayesdm_data.R path/to/hBayesDM [outdir]
# The script needs data.table and checks the SHA-256 of each Stan file before
# writing anything.
#
# The models have no #include lines, so each .stan file is a verbatim copy of
# inst/stan_files/<model>.stan.
#
# The data. Each model has an example data file in inst/extdata and a
# `<task>_preprocess_func` in R/preprocess_funcs.R that turns the table into
# the list Stan receives. hBayesDM_model() (R/hBayesDM_model.R) does two
# things before it calls that function: it lower-cases the column names with
# underscores removed, and it builds `general_info` from the trial counts per
# subject. The script repeats those two steps for the models without blocks
# (the `model_type == ""` branch of R/hBayesDM_model.R) and sources the
# package's own preprocess functions unchanged. igt_pvl_delta's
# `payscale` argument is 100, the value in R/igt_pvl_delta.R.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) stop("usage: gen_hbayesdm_data.R PACKAGE_SOURCE_DIR [OUTDIR]")
PKG <- args[1]
OUT <- if (length(args) >= 2 && nzchar(args[2])) args[2] else "tests/hbayesdm"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))
suppressPackageStartupMessages(library(data.table))

stopifnot(read.dcf(file.path(PKG, "DESCRIPTION"), "Version")[1] == "2.0.1")

MODELS <- list(
  gng_m4 = list(
    sha = "c7c805de439c42438fa1b95f2eee3e120f3c0944cfe357049f4c81b056bf8bcb", data = "gng_exampleData.txt", func = "gng_preprocess_func"),
  prl_rp = list(
    sha = "b41c7641de03d42c961e729ec2d19ac48f3396eefec87d29921a57393d074872", data = "prl_exampleData.txt", func = "prl_preprocess_func"),
  igt_pvl_delta = list(
    sha = "4f7aad1c967f3b87d3885b77d5241ff9cab2207f8a8f25d4c96cc3231b75bc6a", data = "igt_exampleData.txt", func = "igt_preprocess_func",
    extra = list(payscale = 100)),
  dd_hyperbolic = list(
    sha = "efa6626ea5b026b234055e10a2a135e219806660287fff5523c0b35b4b41ca72", data = "dd_exampleData.txt", func = "dd_preprocess_func"))

sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
sha256 <- function(path) {
  a <- if (sha_tool == "shasum") c("-a", "256", path) else path
  strsplit(system2(sha_tool, a, stdout = TRUE), " ")[[1]][1]
}

source(file.path(PKG, "R", "preprocess_funcs.R"))

for (name in names(MODELS)) {
  m <- MODELS[[name]]
  src <- file.path(PKG, "inst", "stan_files", paste0(name, ".stan"))
  if (sha256(src) != m$sha)
    stop(name, ".stan does not match the recorded hash")
  stan <- file.path(OUT, paste0("hbayesdm_", name, ".stan"))
  file.copy(src, stan, overwrite = TRUE)

  raw_data <- fread(file.path(PKG, "inst", "extdata", m$data))
  colnames(raw_data) <- tolower(gsub("_", "", colnames(raw_data), fixed = TRUE))
  DT_trials <- raw_data[, .N, by = "subjid"]
  general_info <- list(subjs = DT_trials$subjid, n_subj = nrow(DT_trials),
                       b_subjs = NULL, b_max = NULL,
                       t_subjs = DT_trials$N, t_max = max(DT_trials$N))
  data_list <- do.call(get(m$func), c(list(raw_data, general_info), m$extra))
  write_json(data_list, paste(readLines(stan), collapse = "\n"),
             file.path(OUT, paste0("hbayesdm_", name, ".json")))
  cat(name, sha256(src), "\n")
}
