#!/usr/bin/env Rscript
# Regenerates tests/bmgarch: two of bmgarch's multivariate GARCH models with
# the package's bundled `panas` data.
#
# bmgarch (https://cran.r-project.org/package=bmgarch) is GPL (>= 3), version
# 2.1.0. This script reads the unpacked CRAN source tarball, so nothing needs
# to be installed or compiled:
#   curl -O https://cran.r-project.org/src/contrib/bmgarch_2.1.0.tar.gz
#   tar -xzf bmgarch_2.1.0.tar.gz          # into an empty directory
#   Rscript tools/gen_bmgarch_data.R path/to/bmgarch [outdir]
#
# The models. inst/stan/CCCMGARCH.stan and BEKKMGARCH.stan are assembled from
# `#include /path` lines that name files under inst/stan. The corpus pairs one
# self-contained .stan file with one .json file, so the script replaces each
# #include line by the text of the file it names, recursively and unchanged,
# and writes the result.
#
# The data. bmgarch() calls the internal function standat() in R/bmgarch.R to
# build the Stan data list. The script evaluates that function from the package
# source and calls it the way the package's example does, on data(panas) (200
# observations of two series): `bmgarch(panas, parameterization = ..., P = 1,
# Q = 1, meanstructure = ..., distribution = "Student_t")`, with the default
# standardize_data = FALSE and xC = NULL. The CCC entry uses the default
# constant mean structure and the BEKK entry uses the ARMA(1,1) mean structure
# that the package's example uses.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) stop("usage: gen_bmgarch_data.R PACKAGE_SOURCE_DIR [OUTDIR]")
PKG <- args[1]
OUT <- if (length(args) >= 2 && nzchar(args[2])) args[2] else "tests/bmgarch"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

stopifnot(read.dcf(file.path(PKG, "DESCRIPTION"), "Version")[1] == "2.1.0")
STAN <- file.path(PKG, "inst", "stan")

inline <- function(path) {
  out <- character()
  for (line in readLines(path)) {
    m <- regmatches(line, regexec("^\\s*#include\\s+(\\S+)\\s*$", line))[[1]]
    if (length(m)) {
      out <- c(out, inline(file.path(STAN, sub("^/", "", m[2]))))
    } else {
      out <- c(out, line)
    }
  }
  out
}

exprs <- parse(file.path(PKG, "R", "bmgarch.R"), keep.source = FALSE)
for (e in exprs) {
  if (is.call(e) && identical(e[[1]], as.name("<-")) &&
      identical(e[[2]], as.name("standat"))) eval(e)
}
stopifnot(exists("standat"))
load(file.path(PKG, "data", "panas.rda"))

MODELS <- list(
  ccc = list(file = "CCCMGARCH.stan", meanstructure = "constant"),
  bekk_arma = list(file = "BEKKMGARCH.stan", meanstructure = "arma"))

for (name in names(MODELS)) {
  m <- MODELS[[name]]
  code <- inline(file.path(STAN, m$file))
  stan <- file.path(OUT, paste0("bmgarch_", name, ".stan"))
  writeLines(code, stan)
  data <- standat(panas, NULL, 1, 1, FALSE, distribution = 1,
                  meanstructure = m$meanstructure)
  data <- data[c("T", "xC", "rts", "nt", "distribution", "P", "Q", "meanstructure")]
  data$xC <- lapply(seq_len(nrow(data$xC)), function(i) data$xC[i, ])
  data$rts <- lapply(seq_len(nrow(data$rts)), function(i) data$rts[i, ])
  write_json(data, paste(code, collapse = "\n"),
             file.path(OUT, paste0("bmgarch_", name, ".json")))
  cat(name, "\n")
}
