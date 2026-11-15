#!/usr/bin/env Rscript
# Regenerates tests/rstanarm: four of rstanarm's Stan models with their
# #include lines inlined, and the Stan data rstanarm builds for one small
# example of each.
#
# rstanarm (https://cran.r-project.org/package=rstanarm) is GPL (>= 3),
# Copyright (C) 2015, 2016 2017 Trustees of Columbia University. This script
# drives the installed package, version 2.32.2 from CRAN:
#   Rscript -e 'remotes::install_version("rstanarm", "2.32.2")'
# The install compiles the package's eight Stan models, which takes minutes.
# The script checks every Stan source file it reads against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The models. Each top-level model in inst/stan pulls in shared pieces with
# `#include /dir/file.stan` lines, some indented and some nested. The corpus
# pairs one self-contained .stan file with one .json file, so each line that
# is an #include is replaced by the full text of the file it names, applied
# recursively. Nothing else is changed, with one exception in continuous.stan:
#
#   vector CODOLS(matrix X, vector y); // implemented in C++
#
# rstanarm implements CODOLS in C++ (inst/include/CODOLS.hpp) as Eigen's
# CompleteOrthogonalDecomposition solve, the least-squares coefficients of y
# on X. stanc rejects a function declaration without a body, so that line is
# replaced by a Stan definition that computes the same least-squares solution
# with a thin QR decomposition. The two agree whenever X has full column
# rank; the script checks that for the data it writes. CmdStan runs the same
# stand-in when the references are recorded. continuous.stan calls CODOLS
# once, in transformed data, when can_do_OLS is 1 (gaussian family, identity
# link, no group terms, no offset, dense X, N > 2).
#
# The data. Each example calls an rstanarm fitting function. rstanarm builds
# the data list and passes it to rstan::sampling(); a method on that generic
# defined here records the list and stops before any sampling happens. The
# data-block fields of that list are written with tools/stan_data_json.R.
#
# Usage:
#   Rscript tools/gen_rstanarm_data.R [outdir]     # default tests/rstanarm

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/rstanarm"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "bernoulli.stan" =
    "5c171f5c1dcc9dc378eb4267bdd6c55c16911d4538d4b5f104e6ac0c9667c919",
  "include/Columbia_copyright.stan" =
    "4a6ae68ab0159f6aabd53ed7c64b7fb517e1d11111b71a96f155004acd6ef0da",
  "include/license.stan" =
    "38cde8b3b39557351231aca32dabfa416d003fbaee6c5830d9bfbcfe6e83ccbd",
  "functions/common_functions.stan" =
    "1990d293a0ef7b73af49b1a0177e62408c490ccdbcee3cf13eaa74a473eb43b9",
  "functions/bernoulli_likelihoods.stan" =
    "529bcdd2a46ea50e0f214f43303ceba53de702ebc63f1a6f155edb0a0620ba1d",
  "data/data_glm.stan" =
    "4746e1956f812533d74c2bb72a1608b73ccb6a8987bb2d95fcf1b78c5441d9e3",
  "data/hyperparameters.stan" =
    "48bd9d9a69fd084c1ee171ea0928ff3f9c90a5f5f806f26a446313a9873fce00",
  "data/glmer_stuff.stan" =
    "1eaa4f6655644ba107209b00d94defe7ca75b127bae99c3165e81af475d5df2a",
  "tdata/tdata_glm.stan" =
    "3bbd705f47de6743f62a8b57d9c6e89c003cc21eb8de9bcf359a46111edd3a31",
  "parameters/parameters_glm.stan" =
    "a732967fa35ca445033426563fd09a4132685da86b2239d9c1790f3d06a75353",
  "tparameters/tparameters_glm.stan" =
    "3a7275b8cb716c0da5fa84b50a27fb04cf32ad7dcad07f205d6ef898c2324428",
  "model/make_eta_bern.stan" =
    "e0199d40afa1069a341fdf8b0ef02819b01121a23fe8103c6132052fdcab7ddc",
  "model/priors_glm.stan" =
    "567d0c51bad6da81bcc0d0a3e6a124d5811b50328c1112d6bfc7277f355fdae5",
  "count.stan" =
    "adfdad3d384f0b80f807253530dd29f56d7c04e46226072899cb01921de036d9",
  "functions/count_likelihoods.stan" =
    "0a1351c83a4d30ad68cb986af79550df62479d10d1abd70114bb343bc064e3ac",
  "data/NKX.stan" =
    "ca614536b96362d9fc7ec6e6fe88a8c80fa554ed7842a1a4e32d3ae18615f099",
  "data/weights_offset.stan" =
    "5efd704c67ce11116f48b1f041edbe80d5c1c410852d76110cbc4f77c1d362a5",
  "data/glmer_stuff2.stan" =
    "8ad958d2cc40cee940d0036503d987cb00cfbd5601ecb8f088eb4ba7bb70f976",
  "model/make_eta.stan" =
    "1d521b2c34dde6fd672b77e4da0fbb8d0f63a7c60788d9b78c3e9bd364d20a35",
  "model/eta_add_Zb.stan" =
    "ff4c756e65b0eea0510fd0b5df4fcd97ba95ea61cd1e258c9a62ad7b287f2f06",
  "model/eta_no_intercept.stan" =
    "36be10d242f914908a5fe1e649318c1a4b3fff1777ad70e62e725df3b7ef0305",
  "polr.stan" =
    "46a8b47e194af6232c81b482f094fbfd022268fd6f0f1f0ea137920898b7182b",
  "continuous.stan" =
    "3c70cca332a1f4b13e5ef0f5fae12e0adfda0bc8a72058a90c9415a1e475c9a4",
  "functions/continuous_likelihoods.stan" =
    "627235a7a9297d1ff4d6b299f9fcdb2bfdca87146a3f34167713c28c2e1e77df",
  "functions/SSfunctions.stan" =
    "f967fb67c5af48fc35f912035fc25f16cb4c4071e83f7f576ff25ae8631582a2",
  "data/data_betareg.stan" =
    "0c888bc5b175be219d838586e684f771493efe8b5e0056b831f30b3205a5c158",
  "tdata/tdata_betareg.stan" =
    "f64d5d724b99a9171f62d4b74078b4af1db4fecdd8a7fb3926bd80babe95ff04",
  "parameters/parameters_betareg.stan" =
    "86acbec3a5d1f81f3a99350bafa87df03b694ea6927e07170b2271e55e587e71",
  "tparameters/tparameters_betareg.stan" =
    "c95bf26a8f1995987bc2fe333ab1fff2610628f948bf723f993f56ace60a4cb1",
  "model/make_eta_z.stan" =
    "46a5c0d0b30c80e53f60b286b8ec44fc5a0212664a4e9deab2285fb303ae2c73",
  "model/eta_z_no_intercept.stan" =
    "28876c9bf6d25afeb1b9fcf09bd1a452085f74fb2029973de8016dbc22f52f04",
  "model/priors_betareg.stan" =
    "8441ab6c5843f51d78a1e205c54440dfa8551f9db8722b683ceebe1b652fc0dc")

for (p in c("rstanarm", "rstan", "lme4", "MASS")) {
  if (!requireNamespace(p, quietly = TRUE)) stop(p, " is not installed")
}
suppressPackageStartupMessages({
  library(rstan)
  library(rstanarm)
})
stopifnot(packageDescription("rstanarm")$Version == "2.32.2")

stan_dir <- system.file("stan", package = "rstanarm")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed rstanarm has no inst/stan/", rel)
  if (is.na(SHA256[rel])) stop("no recorded hash for inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed rstanarm's ", rel, " does not match 2.32.2\n",
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

CODOLS_DECL <- "  vector CODOLS(matrix X, vector y); // implemented in C++"
CODOLS_DEF <- c(
  "  vector CODOLS(matrix X, vector y) {",
  "    return mdivide_left(qr_thin_R(X), qr_thin_Q(X)' * y);",
  "  }")

model_lines <- function(file) {
  lines <- inline(file)
  if (file == "continuous.stan") {
    hit <- which(lines == CODOLS_DECL)
    if (length(hit) != 1) stop("expected one CODOLS declaration in continuous.stan")
    lines <- c(lines[seq_len(hit - 1)], CODOLS_DEF, lines[-seq_len(hit)])
  }
  if (any(grepl("^\\s*#include", lines))) stop(file, ": an #include is left")
  lines
}

cap <- new.env()
setMethod(rstan::sampling, "stanmodel", function(object, ...) {
  cap$model <- object@model_name
  cap$data <- list(...)$data
  stop("standata captured")
}, where = globalenv())
capture <- function(expr) {
  cap$model <- NULL
  cap$data <- NULL
  res <- try(suppressWarnings(suppressMessages(expr)), silent = TRUE)
  if (is.null(cap$data)) stop("rstanarm did not reach rstan::sampling: ", res)
  list(model = cap$model, data = cap$data)
}

verb <- lme4::VerbAgg
verb <- verb[as.integer(verb$id) <= 10, ]
verb$id <- droplevels(verb$id)

examples <- list(
  rstanarm_glmer_bernoulli = list(file = "bernoulli.stan", run = function()
    stan_glmer(r2 ~ Anger + Gender + btype + (1 | id) + (1 + Gender | item),
               data = verb, family = binomial(link = "logit"))),
  rstanarm_glmer_nb_count = list(file = "count.stan", run = function()
    stan_glmer.nb(Days ~ Sex/(Age + Eth*Lrn) + (1 | Eth:Sex:Lrn),
                  data = MASS::quine)),
  rstanarm_polr = list(file = "polr.stan", run = function()
    stan_polr(tobgp ~ agegp, data = esoph, prior = R2(0.2, "mean"),
              init_r = 0.1)),
  rstanarm_glm_gaussian = list(file = "continuous.stan", run = function()
    stan_glm(mpg ~ wt + am, data = mtcars)))

# decl_ndim() cuts a declaration at its first "=", which drops fields whose
# array size uses a comparison, like `array[prior_dist == 7 ? K : 0] int
# num_normals`. It only parses the text, so comparisons are masked first.
parse_code <- function(code) {
  for (op in c("==", "!=", "<=", ">=")) code <- gsub(op, " ~ ", code, fixed = TRUE)
  code
}

flag <- function(sd, k) if (is.null(sd[[k]])) "-" else paste(as.integer(sd[[k]]), collapse = ",")

for (id in names(examples)) {
  ex <- examples[[id]]
  code_lines <- model_lines(ex$file)
  code <- paste(code_lines, collapse = "\n")
  data_fields <- names(decl_ndim(parse_code(code)))
  set.seed(20261114)
  got <- capture(ex$run())
  if (got$model != sub("\\.stan$", "", ex$file)) {
    stop(id, ": rstanarm chose ", got$model, ", expected ", ex$file)
  }
  sd <- got$data
  # rstan lets a data-block field be absent when its declared size is 0, and
  # rstanarm relies on that (stan_polr passes its weights as `wt`, so the
  # model's `weights` is never set). Those fields are written as [];
  # CmdStan rejects the file if any of them has a nonzero declared size.
  absent <- setdiff(data_fields, names(sd))
  for (k in absent) sd[[k]] <- numeric(0)
  sd <- sd[data_fields]
  for (k in data_fields) if (length(sd[[k]]) == 0) sd[[k]] <- numeric(0)
  if (ex$file == "continuous.stan") {
    X_ <- cbind(if (sd$has_intercept) 1, sd$X[1, , ])
    if (qr(X_)$rank != ncol(X_)) stop(id, ": X is rank deficient")
  }
  writeLines(code_lines, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, parse_code(code), file.path(OUT, paste0(id, ".json")))
  if (length(absent)) cat(id, "absent fields written as []:", absent, "\n")
  cat(sprintf("%-26s N=%s K=%s t=%s special_case=%s num_non_zero=%s dense_X=%s family=%s\n",
              id, flag(sd, "N"), flag(sd, "K"), flag(sd, "t"),
              flag(sd, "special_case"), flag(sd, "num_non_zero"),
              flag(sd, "dense_X"), flag(sd, "family")))
}
