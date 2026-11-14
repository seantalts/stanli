#!/usr/bin/env Rscript
# Regenerates tests/hdbayes: two of hdbayes's Stan models, lm_npp.stan and
# pwe_npp.stan, with the Stan data the package's own R functions build for
# its example data sets.
#
# hdbayes (https://cran.r-project.org/package=hdbayes) is MIT licensed,
# Copyright (c) 2024 hdbayes authors. This script drives the installed
# package, version 0.2.0 from CRAN:
#   Rscript -e 'remotes::install_version("hdbayes", "0.2.0")'
# hdbayes compiles its models with instantiate at install time; when no
# CmdStan is configured that step is skipped and the package still installs,
# which is all this script needs.
# The package copies src/stan/ to bin/stan/ in the installed tree. The script
# checks those files against the hashes below before writing anything.
#
# The models have no #include lines, so each .stan file is a verbatim copy.
#
# The data. lm.npp() and pwe.npp() build a list and pass it to
# instantiate::stan_package_model(...)$sample(data = ...). The script
# replaces stan_package_model with a stub whose $sample() signals that list
# back, so the data comes from the package's code and nothing is sampled.
#
# pwe.npp() takes the normalized power prior's log normalizing constant as
# two user inputs, a0.lognc (a grid of a0 values) and lognc (its value at
# each grid point). The package's example estimates lognc with
# pwe.npp.lognc(), which runs MCMC and bridge sampling at each grid point, so
# the values need CmdStan and change from run to run. LOGNC below holds one
# such estimate, made once on 2026-11-14 for the example's grid
# seq(0, 1, length.out = 11): pwe.npp.lognc() on the historical data with
# iter_warmup = 500, iter_sampling = 1000, chains = 1, seed = 123, CmdStan
# 2.40.0, cmdstanr 0.9.0, bridgesampling 1.2.1.
#
# Usage:
#   Rscript tools/gen_hdbayes_data.R [outdir]     # default tests/hdbayes

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/hdbayes"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "lm_npp.stan" =
    "6ff96d0d018a21ac1a11258b5feac0698949f13ad698458f6eac9522cded49d5",
  "pwe_npp.stan" =
    "3f5b39b482a83eab7076fb4cc3b7f0b278a2b987fa3c895998d6c8f370d0c5a3")

if (!requireNamespace("hdbayes", quietly = TRUE)) {
  stop("hdbayes is not installed; install version 0.2.0 from CRAN.")
}
stopifnot(packageDescription("hdbayes")$Version == "0.2.0")

stan_dir <- system.file("bin", "stan", package = "hdbayes")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed hdbayes has no bin/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed hdbayes's ", rel, " does not match 0.2.0\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}

capture_standata <- function(expr) {
  stub <- function(name, package, ...) {
    list(sample = function(data, ...) {
      stop(structure(class = c("hdbayes_standata", "condition"),
                     list(message = "standata captured", call = NULL,
                          data = data)))
    })
  }
  ns <- asNamespace("instantiate")
  orig <- get("stan_package_model", envir = ns)
  utils::assignInNamespace("stan_package_model", stub, ns = "instantiate")
  on.exit(utils::assignInNamespace("stan_package_model", orig,
                                   ns = "instantiate"))
  tryCatch({
    expr
    stop("the hdbayes function returned without calling $sample()")
  }, hdbayes_standata = function(c) c$data)
}

write_entry <- function(id, model, sd) {
  code <- paste(model, collapse = "\n")
  fields <- names(decl_ndim(code))
  missing_fields <- setdiff(fields, names(sd))
  if (length(missing_fields) > 0) {
    stop(id, ": standata is missing data-block fields: ",
         paste(missing_fields, collapse = ", "))
  }
  sd <- sd[fields]
  writeLines(model, file.path(OUT, paste0(id, ".stan")))
  write_json(sd, code, file.path(OUT, paste0(id, ".json")))
  sd
}

env <- new.env()
utils::data(list = c("actg019", "actg036", "E1684", "E1690"),
            package = "hdbayes", envir = env)

# lm.npp: the subsets and formula of the package's glm.npp() example.
lm_model <- read_checked("lm_npp.stan")
sd <- capture_standata(hdbayes::lm.npp(
  formula = cd4 ~ treatment + age + race,
  data.list = list(currdata = env$actg019[1:100, ],
                   histdata = env$actg036[1:50, ])))
sd <- write_entry("hdbayes_lm_npp", lm_model, sd)
cat(sprintf("%-22s n=%d n0=%s p=%d K=%d\n", "hdbayes_lm_npp",
            sd$n, paste(sd$n0s, collapse = ","), sd$p, sd$K))

# pwe.npp: the package's own example setup.
E1684 <- env$E1684[1:100, ]
E1690 <- env$E1690[1:50, ]
E1684$failtime[E1684$failtime == 0] <- 0.50 / 365.25
E1690$failtime[E1690$failtime == 0] <- 0.50 / 365.25
E1684$cage <- as.numeric(scale(E1684$age))
E1690$cage <- as.numeric(scale(E1690$age))
nbreaks <- 3
breaks <- as.numeric(quantile(E1690[E1690$failcens == 1, ]$failtime,
                              probs = 1:nbreaks / nbreaks))
breaks <- c(0, breaks)
breaks[length(breaks)] <- max(10000, 1000 * breaks[length(breaks)])
a0 <- seq(0, 1, length.out = 11)
LOGNC <- c(-0.046222111101053009, -31.216381669855924, -48.459238547312601,
           -64.280718551799296, -79.576822737569145, -94.530572572819651,
           -109.2603549476391, -123.92024195832984, -138.44846184564696,
           -152.9340922307305, -167.35310563290636)
pwe_model <- read_checked("pwe_npp.stan")
sd <- capture_standata(hdbayes::pwe.npp(
  formula = survival::Surv(failtime, failcens) ~ treatment + sex + cage +
    node_bin,
  data.list = list(currdata = E1690, histdata = E1684),
  a0.lognc = a0,
  lognc = LOGNC,
  breaks = breaks))
sd <- write_entry("hdbayes_pwe_npp", pwe_model, sd)
cat(sprintf("%-22s n1=%d n0=%d p=%d J=%d s=%d\n", "hdbayes_pwe_npp",
            sd$n1, sd$n0, sd$p, sd$J, sd$s))
