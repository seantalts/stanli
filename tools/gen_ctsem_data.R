#!/usr/bin/env Rscript
# Generate a synthetic panel dataset for ctsem's generic Stan template
# (tests/ctsem/ctsem_ctsm.stan), in the JSON shape stanli's data loader
# expects. Default 40 subjects x 8 time points.
#
# ctsem (https://github.com/cdriveraus/ctsem) is GPL-3. This script drives the
# installed ctsem *package* (installed separately by whoever runs this) to
# build a model object and simulate data, then extracts the standata list
# ctsem itself constructs on the way to fitting and writes it out as JSON.
# The model file in tests/ctsem is ctsem's inst/stan/ctsm.stan, unmodified.
#
# Pinned revision (matches tools/repro_ctsem_stanc3.sh):
#   cdriveraus/ctsem @ a0f1e69b7282c1dcaed820919ae0d8b280d076c4
#   inst/stan/ctsm.stan, SHA-256 b148f5b3f129f981e7a3bfbd966e825c28fbc7314d14e2896d18c7c6bc1b01d2
#
# The CRAN release does not match this pin (verified 2026-09-03: CRAN ctsem
# 3.11.1's ctsm.stan hashes to 12185b6c7268b345c014aad05080a75cf46dbeafc9bd3ecc54c8ffb4d99eaabe).
# Install the pinned revision explicitly:
#   Rscript -e 'remotes::install_github("cdriveraus/ctsem@a0f1e69b7282c1dcaed820919ae0d8b280d076c4")'
# This script verifies the installed package's ctsm.stan against the pinned
# hash before doing anything else, and aborts on a mismatch.
#
# Usage:
#   Rscript tools/gen_ctsem_data.R OUT.json [--seed N] [--subjects N]
#
# Model and data-generation choices (documented, not arbitrary):
#   - Generating model: the two-process "leading indicator" example from
#     ctsem's own ?ctGenerate help page (man/ctGenerate.Rd) -- two latent
#     processes, one noisy manifest indicator each, individual differences
#     in the process intercepts (CINT) via a Cholesky-structured trait
#     distribution. This is ctsem's own documented worked example, just
#     scaled from 15 subjects to 40.
#   - Tpoints = 8: the value used in that same ?ctGenerate example.
#   - Fitting model: the matching Stan-side ctModel(type='ct', ...) -- CINT
#     free (individually varying by ctsem's own default for type='ct'),
#     MANIFESTMEANS fixed to 0 (freeing both CINT and MANIFESTMEANS is
#     unidentified), DRIFT/DIFFUSION/T0MEANS/T0VAR/MANIFESTVAR left at
#     ctsem's free defaults.
#   - ctFit(..., fit=FALSE, optimize=FALSE, priors=TRUE): optimize=FALSE
#     because we want the HMC-shaped standata (intoverpop defaults to FALSE
#     for HMC per ctsem's own 'auto' rule, so per-subject random effects
#     enter the parameter vector -- the real hierarchical NUTS workload).
#     priors=TRUE because ctFit.Rd's own examples fit with priors=TRUE and
#     a hierarchical model without priors is not reliably identified for
#     NUTS. fit=FALSE stops short of calling into rstan/Stan at all; we only
#     want the standata list ctsem builds on the way there.

suppressPackageStartupMessages({
  library(jsonlite)
})

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("usage: gen_ctsem_data.R OUT.json [--seed N] [--subjects N]")
}
out_path <- args[[1]]
opt <- function(flag, default) {
  i <- which(args == flag)
  if (length(i) == 0) return(default)
  args[[i + 1]]
}
seed <- as.integer(opt("--seed", "20260903"))
n_subjects <- as.integer(opt("--subjects", "40"))
tpoints <- 8L # ctsem ?ctGenerate example value; see header comment

CTSEM_REV <- "a0f1e69b7282c1dcaed820919ae0d8b280d076c4"
CTSM_STAN_SHA256 <- "b148f5b3f129f981e7a3bfbd966e825c28fbc7314d14e2896d18c7c6bc1b01d2"

if (!requireNamespace("ctsem", quietly = TRUE)) {
  stop("ctsem is not installed. Install the pinned revision with:\n",
       '  Rscript -e \'remotes::install_github("cdriveraus/ctsem@',
       CTSEM_REV, '")\'')
}
suppressPackageStartupMessages(library(ctsem))

stan_path <- system.file("stan", "ctsm.stan", package = "ctsem")
if (!nzchar(stan_path) || !file.exists(stan_path)) {
  stop("installed ctsem package has no inst/stan/ctsm.stan")
}
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
sha_args <- if (sha_tool == "shasum") c("-a", "256", stan_path) else stan_path
actual_sha256 <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
if (actual_sha256 != CTSM_STAN_SHA256) {
  stop("installed ctsem's ctsm.stan does not match pinned revision ",
       CTSEM_REV, "\n  expected sha256 ", CTSM_STAN_SHA256,
       "\n  actual   sha256 ", actual_sha256,
       "\nInstall the pinned revision (see header comment) rather than",
       " whatever CRAN or a different ref provides.")
}
cat("ctsm.stan hash OK:", actual_sha256, "\n")

set.seed(seed)

# ---- generating model: bivariate leading-indicator, per ?ctGenerate ----
generatingModel <- ctModel(
  Tpoints = tpoints, n.latent = 2, n.TDpred = 0, n.TIpred = 0, n.manifest = 2,
  MANIFESTVAR = diag(.1, 2),
  LAMBDA = diag(1, 2),
  DRIFT = matrix(c(-.2, -.05, -.1, -.1), nrow = 2),
  DIFFUSION = matrix(c(1, .2, 0, 4), 2),
  CINT = matrix(c(1, 0), nrow = 2),
  T0MEANS = matrix(0, ncol = 1, nrow = 2),
  T0VAR = diag(1, 2))

traitChol <- matrix(c(.5, .2, 0, .8), nrow = 2)
subjectCint <- t(replicate(n_subjects, as.numeric(traitChol %*% rnorm(2))))
datalist <- vector("list", n_subjects)
for (i in seq_len(n_subjects)) {
  subjectModel <- generatingModel
  subjectModel$CINT <- matrix(subjectCint[i, ], ncol = 1)
  d <- ctGenerate(subjectModel, n.subjects = 1, burnin = 10)
  d[, "id"] <- i
  datalist[[i]] <- d
}
datalong <- do.call(rbind, datalist)
cat("simulated", nrow(datalong), "rows,", n_subjects, "subjects,",
    tpoints, "timepoints/subject\n")

# ---- fitting model: Stan-side ctModel matching the generator ----
fittingModel <- ctModel(
  type = "ct",
  latentNames = c("eta1", "eta2"),
  manifestNames = c("Y1", "Y2"),
  LAMBDA = diag(1, 2),
  MANIFESTVAR = diag(.1, 2),
  CINT = matrix(c("cint1", "cint2"), ncol = 1),
  MANIFESTMEANS = matrix(0, nrow = 2)) # fixed: CINT is already free+indvarying

fit <- ctFit(datalong = datalong, model = fittingModel,
             fit = FALSE, optimize = FALSE, priors = TRUE)
standata <- fit$standata

# ---- filter to exactly ctsm.stan's data block, in its field order ----
data_block_fields <- c(
  "ndatapoints", "nmanifest", "nlatent", "nlatentpop", "nsubjects",
  "ntipred", "ntdpred", "tipredsdata", "nmissingtipreds", "ntipredeffects",
  "tipredsimputedscale", "tipredeffectscale", "Y", "priors", "tdpreds",
  "maxtimestep", "time", "subject", "nparams", "continuoustime",
  "nindvarying", "nindvaryingoffdiagonals", "sdscale", "rawpopcovscale",
  "indvaryingindex", "notindvaryingindex", "nobs_y", "whichobs_y",
  "ndiffusion", "derrind", "manifesttype", "nbinary_y", "whichbinary_y",
  "ncont_y", "whichcont_y", "intoverpop", "statedep", "choleskymats",
  "intoverstates", "verbose", "TIPREDEFFECTsetup", "nrowmatsetup",
  "matsetup", "matvalues", "whenmat", "whenvecp", "whenvecs", "matrixdims",
  "savescores", "savesubjectmatrices", "dokalman", "dokalmanrows",
  "nsubsets", "Jstep", "priormod", "intoverpopindvaryingindex",
  "nJAxfinite", "JAxfinite", "nJyfinite", "Jyfinite", "taylorheun",
  "popcovn", "llsinglerow", "laplaceprior", "laplaceprioronly",
  "laplacetipreds", "CINTnonzerosize", "CINTnonzero", "nDRIFTsubsets",
  "nJAxsubsets", "DRIFTsubsets", "JAxsubsets")

missing_fields <- setdiff(data_block_fields, names(standata))
if (length(missing_fields) > 0) {
  stop("ctFit()'s standata is missing fields ctsm.stan's data block ",
       "declares: ", paste(missing_fields, collapse = ", "))
}

# scalar (int/real) fields in the data block -- everything else is a
# declared array/vector/matrix and must serialize as a JSON array even
# when its extent is 0 or 1.
scalar_fields <- c(
  "ndatapoints", "nmanifest", "nlatent", "nlatentpop", "nsubjects",
  "ntipred", "ntdpred", "nmissingtipreds", "ntipredeffects",
  "tipredsimputedscale", "tipredeffectscale", "priors", "maxtimestep",
  "nparams", "continuoustime", "nindvarying", "nindvaryingoffdiagonals",
  "rawpopcovscale", "ndiffusion", "intoverpop", "choleskymats",
  "intoverstates", "verbose", "nrowmatsetup", "savescores",
  "savesubjectmatrices", "dokalman", "nsubsets", "Jstep", "priormod",
  "nJAxfinite", "nJyfinite", "taylorheun", "popcovn", "llsinglerow",
  "laplaceprioronly", "laplacetipreds", "CINTnonzerosize",
  "nDRIFTsubsets", "nJAxsubsets")

out <- list()
for (f in data_block_fields) {
  v <- standata[[f]]
  if (f %in% scalar_fields) {
    out[[f]] <- unbox(as.vector(v))
  } else if (length(v) == 0) {
    # stanli's data loader only compares the declared type's total element
    # count against the JSON array's total element count (runtime/src/
    # lower.cpp validate_data_dims / bind_data); it discards whatever
    # nested shape the JSON had and rebuilds dims from the declaration.
    # So any zero-total-size 1D/2D field -- a zero-row matrix, a zero-col
    # matrix, or an empty vector -- must serialize as a bare `[]`, never
    # as N nested empty rows (jsonlite's default for e.g. matrix(4,0)).
    out[[f]] <- list()
  } else {
    out[[f]] <- v
  }
}

json <- toJSON(out, auto_unbox = FALSE, digits = NA, null = "null")
writeLines(json, out_path)

sz <- file.info(out_path)$size
cat(sprintf("wrote %s (%d bytes, %.1f KiB)\n", out_path, sz, sz / 1024))
