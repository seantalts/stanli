#!/usr/bin/env Rscript
# Regenerates tests/oncobayes2: OncoBayes2's blrm_exnex.stan with its one
# #include inlined, and the Stan data of the package's three example models.
#
# OncoBayes2 (https://cran.r-project.org/package=OncoBayes2) is GPL (>= 3),
# Copyright (C) 2025 Novartis Pharma AG. This script drives the installed
# package (installed separately by whoever runs this):
#   Rscript -e 'install.packages("OncoBayes2")'   # must be 0.10-0
# It checks the installed package's Stan sources against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The model. inst/stan/blrm_exnex.stan contains one include,
# `#include /include/utils.stan`, on its third line. The corpus pairs one
# self-contained .stan file with one .json file, so the checked-in model is
# blrm_exnex.stan with that line replaced by the full text of
# inst/stan/include/utils.stan, preceded by the package's two header files,
# inst/stan/include/copyright_novartis.stan and inst/stan/include/license.stan
# (both are comments only; rstantools puts license.stan at the top of the
# generated C++). Nothing else is changed. The same text is written three
# times, once per dataset, because the corpus inventory pairs file stems.
#
# The data. example_model(topic) runs the package's own example for
# "single_agent", "combo2" and "combo3"; each leaves a `blrmfit` object whose
# $standata is the list passed to Stan. The data-block fields of that list
# are written with tools/stan_data_json.R. The examples sample with the
# OncoBayes2.MC.* options; they are set small here because only the standata
# is used, and the standata does not depend on them.
#
# Usage:
#   Rscript tools/gen_oncobayes2_data.R [outdir]     # default tests/oncobayes2

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/oncobayes2"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "blrm_exnex.stan" =
    "018a44f4a830410f08e1a9ef9beef45fd6419cec27f5703f7c46860c44e56bc0",
  "include/utils.stan" =
    "ee3512fada223fd2f3135fadf3a2a72767c118e13ab2bc0a5da5037933b7f945",
  "include/copyright_novartis.stan" =
    "f846dfd08671fdbd1e0b7e71ec5879bf84a67f4aab0d5d6d97d2f2656f9ea464",
  "include/license.stan" =
    "49db4c73c23bfadf9c2b9f55773fd944d1a8a64aada3897afdfeb91eeee7f405")

if (!requireNamespace("OncoBayes2", quietly = TRUE)) {
  stop("OncoBayes2 is not installed; install version 0.10-0 from CRAN.")
}
suppressPackageStartupMessages(library(OncoBayes2))
stopifnot(packageDescription("OncoBayes2")$Version == "0.10-0")

stan_dir <- system.file("stan", package = "OncoBayes2")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed OncoBayes2 has no inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed OncoBayes2's ", rel, " does not match 0.10-0\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}
model <- read_checked("blrm_exnex.stan")
utils <- read_checked("include/utils.stan")
copyright <- read_checked("include/copyright_novartis.stan")
license <- read_checked("include/license.stan")

inc <- grep("^#include", model)
if (length(inc) != 1 || trimws(model[inc]) != "#include /include/utils.stan") {
  stop("expected exactly one #include of /include/utils.stan in blrm_exnex.stan")
}
code_lines <- c(copyright, license, model[seq_len(inc - 1)], utils,
                model[-seq_len(inc)])
code <- paste(code_lines, collapse = "\n")

options(OncoBayes2.MC.warmup = 10, OncoBayes2.MC.iter = 20,
        OncoBayes2.MC.chains = 1, OncoBayes2.MC.save_warmup = FALSE,
        mc.cores = 1)

# The data block's field names, in order.
data_fields <- local({
  nd <- decl_ndim(code)
  names(nd)
})

for (topic in c("single_agent", "combo2", "combo3")) {
  id <- paste0("oncobayes2_blrm_exnex_",
               if (topic == "single_agent") "sa" else topic)
  env <- new.env()
  set.seed(20261028)
  suppressWarnings(suppressMessages(
    example_model(topic, envir = env, silent = TRUE)))
  sd <- env$blrmfit$standata
  missing_fields <- setdiff(data_fields, names(sd))
  if (length(missing_fields) > 0) {
    stop(topic, ": standata is missing data-block fields: ",
         paste(missing_fields, collapse = ", "))
  }
  sd <- sd[data_fields]
  # Fields with no elements (num_inter = 0 for a single agent) are written as
  # a bare [], which CmdStan and stanli accept for any declared shape with a
  # zero extent.
  for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
  writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, code, file.path(OUT, paste0(id, ".json")))
  cat(sprintf("%-32s num_obs=%d num_comp=%d num_inter=%d num_groups=%d\n",
              id, sd$num_obs, sd$num_comp, sd$num_inter, sd$num_groups))
}
