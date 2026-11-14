#!/usr/bin/env Rscript
# Regenerates tests/epinow2: EpiNow2's estimate_infections.stan and
# estimate_secondary.stan with their #includes inlined, and the Stan data the
# package builds for one small example of each.
#
# EpiNow2 (https://cran.r-project.org/package=EpiNow2) is MIT licensed,
# Copyright (c) 2020 EpiForecasts. This script drives the installed
# package, version 1.9.0 from CRAN:
#   Rscript -e 'remotes::install_version("EpiNow2", "1.9.0")'
# It checks the installed package's Stan sources against the hashes below
# before writing anything, and aborts on a mismatch.
#
# The models. Each model file includes files from inst/stan/functions and
# inst/stan/data. The checked-in model is the package's inst/stan/include/
# license.stan header (comments only; rstantools puts it at the top of the
# generated C++), then the model file with every `#include` line replaced by
# the text of the file it names. Nothing else is changed.
#
# The data. estimate_infections() and estimate_secondary() build the data list
# and then call the internal fit_model(). fit_model is replaced in the
# namespace by a function that records args$data and stops, so no sampling
# runs. The data-block fields of the recorded list are written with
# tools/stan_data_json.R.
#
# Usage:
#   Rscript tools/gen_epinow2_data.R [outdir]     # default tests/epinow2

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/epinow2"
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

SHA256 <- c(
  "include/license.stan" =
    "67b05049cc98b1d09e2862fe603a832a174cfa248abe3abeaf0048d5872b9276",
  "estimate_infections.stan" =
    "cbeff3c3e08bc3a718796d103ab41197f4764444709810cd54b602523e21a2cb",
  "estimate_secondary.stan" =
    "9663c6a5877f050d8ca8480502f6aaad1387d83e0efc57ce801f2bb3732698e6",
  "data/backcalc.stan" =
    "b1a2da27ffda8e273f33b9f75f958a769ded171d0068adcbea12bcdeafd70b22",
  "data/delays.stan" =
    "c411f5549c1c7525b5d409958a2c7fe93ba8ce6a4c0b4ca868ee128c524887c0",
  "data/estimate_infections_params.stan" =
    "262724f59f148df0f828da6811d8e18461489317ac6c7da0823bc76490be537a",
  "data/estimate_secondary_params.stan" =
    "eedd2a6702fe91a1d4355eeb49ca492f1f40647fe5f79f31fe85063b60f91ff4",
  "data/gaussian_process.stan" =
    "a706b67173f678b239bfda224d8f332cf58143edcf79b512ce2677c846c6f4cf",
  "data/observation_model.stan" =
    "0a4911f4ef80bd75f1e5a4e092092bffe5ca7d2a0a9f7ae262e9b378359b98d4",
  "data/observations.stan" =
    "63b78ca2bc127725c400edac0d1df66a8ccc7e9c975a46ed0841296ea1fa9ce4",
  "data/params.stan" =
    "5e66f1fefc0058292cf87e17a37f396e898b2059c256f8bcf95094acc6928fed",
  "data/rt.stan" =
    "13a46c23310efb7237cd52ac8678a683ccd395a46442afdcd0e20b6ef589a890",
  "data/secondary.stan" =
    "e8b71ccb42abc615bbc464fa1b1e69367f9209c99ee54d608fa6a4fb27bb56df",
  "functions/convolve.stan" =
    "476edeb59934963c68690651a3df86f317140c34b3b01a1ca15160cb10566a33",
  "functions/delays.stan" =
    "81e52bc3516796e5711b59b758e942097425edf4f782c4e03d3308f559a76bce",
  "functions/gaussian_process.stan" =
    "69c9849dca36c469a0a3a7e3d4d970b33c3bc8815f44580783a7d1f64dabf865",
  "functions/generated_quantities.stan" =
    "2744520146ca7acba237cf3aa378de3fc7e177b3f8dc4ae5a34f5074f679f1a2",
  "functions/infections.stan" =
    "caca76a00bed8d47929d1463dfa03626450c3b40289298abc62dc1064b072d08",
  "functions/observation_model.stan" =
    "c2e3a3a46a4bad59dcc28bb57b9db73a52dda05e0ad3ab27ef36a75158ad6592",
  "functions/params.stan" =
    "b85583c32493e3f2b2aece0400d645c6c4f84eab478c00032040dddc4f93b93b",
  "functions/pmfs.stan" =
    "002517f321d869ce6b830fb50a81dfea3a0a7142611bab7fb77e5f0b97f3608c",
  "functions/primarycensored.stan" =
    "2ac1544aa500934f382f5036a83f2b577d44e9afb8c64aeb5406b3f1f81989c8",
  "functions/rt.stan" =
    "49746ef9301bff9806b3bd3a423c5b50ebcb7e5b930c866a73d18378faa24dcf",
  "functions/secondary.stan" =
    "0069e4637f6261fbfbdd619a7f4f26d6e18d220d361272d42814684c57784c5f")

if (!requireNamespace("EpiNow2", quietly = TRUE)) {
  stop("EpiNow2 is not installed; install version 1.9.0 from CRAN.")
}
suppressPackageStartupMessages({
  library(EpiNow2)
  library(data.table)
})
stopifnot(packageDescription("EpiNow2")$Version == "1.9.0")

stan_dir <- system.file("stan", package = "EpiNow2")
sha_tool <- if (nzchar(Sys.which("shasum"))) "shasum" else "sha256sum"
read_checked <- function(rel) {
  path <- file.path(stan_dir, rel)
  if (!file.exists(path)) stop("installed EpiNow2 has no inst/stan/", rel)
  if (is.na(SHA256[rel])) stop("no recorded hash for inst/stan/", rel)
  sha_args <- if (sha_tool == "shasum") c("-a", "256", path) else path
  actual <- strsplit(system2(sha_tool, sha_args, stdout = TRUE), " ")[[1]][1]
  if (actual != SHA256[[rel]]) {
    stop("installed EpiNow2's ", rel, " does not match 1.9.0\n",
         "  expected sha256 ", SHA256[[rel]], "\n  actual   sha256 ", actual)
  }
  readLines(path, warn = FALSE)
}

# The included files contain no #include lines of their own (checked here).
flatten <- function(model_file) {
  lines <- read_checked(model_file)
  out <- character(0)
  for (l in lines) {
    if (grepl("^\\s*#include", l)) {
      rel <- sub("^/", "", trimws(sub("^\\s*#include", "", l)))
      inc <- read_checked(rel)
      if (any(grepl("^\\s*#include", inc))) stop(rel, " has nested includes")
      out <- c(out, inc)
    } else {
      out <- c(out, l)
    }
  }
  c(read_checked("include/license.stan"), out)
}

capture_stan_data <- function(expr) {
  captured <- NULL
  ns <- asNamespace("EpiNow2")
  original <- get("fit_model", envir = ns)
  assignInNamespace("fit_model", function(args, id = "stan") {
    captured <<- args$data
    stop("stan data captured")
  }, ns = "EpiNow2")
  on.exit(assignInNamespace("fit_model", original, ns = "EpiNow2"))
  tryCatch(expr, error = function(e) {
    if (conditionMessage(e) != "stan data captured") stop(e)
  })
  if (is.null(captured)) stop("fit_model was not reached")
  captured
}

write_entry <- function(id, code_lines, sd) {
  code <- paste(code_lines, collapse = "\n")
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
  sd
}

# estimate_infections: the package's own example (generation time, incubation
# period plus reporting delay, Rt prior), on the first 35 days of
# example_confirmed, with a truncation distribution from the trunc_opts()
# example and one Rt breakpoint added so the model's T[0,] prior on bp_sd runs.
reported_cases <- as.data.table(example_confirmed)[1:35]
reported_cases[, breakpoint := as.integer(date == date[20])]
generation_time <- Gamma(shape = Normal(1.3, 0.3), rate = Normal(0.37, 0.09),
                         max = 14)
incubation_period <- LogNormal(meanlog = Normal(1.6, 0.06),
                               sdlog = Normal(0.4, 0.07), max = 14)
reporting_delay <- LogNormal(mean = 2, sd = 1, max = 10)
sd_inf <- capture_stan_data(suppressWarnings(suppressMessages(
  estimate_infections(reported_cases,
    generation_time = gt_opts(generation_time),
    delays = delay_opts(incubation_period + reporting_delay),
    truncation = trunc_opts(LogNormal(mean = 3, sd = 2, max = 10)),
    rt = rt_opts(prior = LogNormal(mean = 2, sd = 0.1)),
    stan = stan_opts(samples = 100, warmup = 200),
    verbose = FALSE))))
sd_inf <- write_entry("epinow2_estimate_infections",
                      flatten("estimate_infections.stan"), sd_inf)
with(sd_inf, stopifnot(fixed == 0, estimate_r == 1, bp_n == 1,
                       delay_id_reporting > 0, delay_id_truncation > 0,
                       delay_params_length > 0, n_params_variable > 0))
cat(sprintf(paste("%-34s t=%d seeding_time=%d M=%d bp_n=%d",
                  "delay_params_length=%d n_params_variable=%d\n"),
            "epinow2_estimate_infections", sd_inf$t, sd_inf$seeding_time,
            sd_inf$M, sd_inf$bp_n, sd_inf$delay_params_length,
            sd_inf$n_params_variable))

# estimate_secondary: the package's incidence example (40 percent of cases
# reported after a lognormal delay), on its first 40 days instead of 60.
cases <- as.data.table(example_confirmed)[, primary := confirm]
cases[, scaling := 0.4]
cases[, meanlog := 1.8][, sdlog := 0.5]
cases <- convolve_and_scale(cases, type = "incidence")
sd_sec <- capture_stan_data(suppressWarnings(suppressMessages(
  estimate_secondary(cases[1:40],
    obs = obs_opts(scale = Normal(mean = 0.2, sd = 0.2), week_effect = FALSE),
    verbose = FALSE))))
sd_sec <- write_entry("epinow2_estimate_secondary",
                      flatten("estimate_secondary.stan"), sd_sec)
with(sd_sec, stopifnot(delay_id_reporting > 0, delay_params_length > 0,
                       n_params_variable > 0))
cat(sprintf("%-34s t=%d lt=%d delay_params_length=%d n_params_variable=%d\n",
            "epinow2_estimate_secondary", sd_sec$t, sd_sec$lt,
            sd_sec$delay_params_length, sd_sec$n_params_variable))
