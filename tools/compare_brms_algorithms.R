# Posterior means and standard deviations of small brms models under each
# inference algorithm of the stanli backend, next to NUTS from the same
# backend and, optionally, rstan::vb() through brms's rstan backend.
#
#   Rscript tools/compare_brms_algorithms.R OUT.csv [rstan]
#
# Needs a brms whose stanli backend dispatches on `algorithm`, a stanli R
# package with variational_model() and friends, and STANLI_RUNTIME. With the
# second argument "rstan" it also compiles three of the models with rstan
# (about a minute and a few GB each, one at a time).
#
# Writes one row per (model, backend, algorithm, seed, variable) with the
# mean and sd of the draws. The formulas are those of
# tools/gen_brms_models.R, on the same simulated data.

suppressMessages(library(brms))
args <- commandArgs(trailingOnly = TRUE)
out_file <- if (length(args)) args[1] else "brms_algorithms.csv"
with_rstan <- length(args) > 1 && args[2] == "rstan"

set.seed(7)
N <- 40
sim <- data.frame(
  x = rnorm(N), z = rnorm(N), g = factor(rep(1:5, each = 8)),
  tt = rep(1:8, 5),
  y = rnorm(N), ypos = rexp(N) + 0.1, cnt = rpois(N, 3),
  bin = rbinom(N, 1, 0.5), tr = rep(10L, N)
)

models <- list(
  sw_gaussian = list(formula = y ~ x + z, data = sim, family = gaussian()),
  sw_bernoulli = list(formula = bin ~ x + z, data = sim, family = bernoulli()),
  sw_poisson = list(formula = cnt ~ x + z, data = sim, family = poisson()),
  sw_negbinomial = list(formula = cnt ~ x + z, data = sim,
                        family = negbinomial()),
  sw_re_gauss = list(formula = y ~ x + (1 | g), data = sim,
                     family = gaussian()),
  sw_re_pois = list(formula = cnt ~ x + (1 | g), data = sim,
                    family = poisson()),
  i319_pois_re = list(formula = count ~ zAge + zBase * Trt + (1 | patient),
                      data = epilepsy, family = poisson())
)
rstan_models <- c("sw_gaussian", "sw_bernoulli", "sw_re_pois")
algorithms <- c("meanfield", "fullrank", "pathfinder", "laplace")
approximation_seeds <- 1:5

rows <- list()
notes <- list()
record <- function(fit, model, backend, algorithm, seed, warnings) {
  draws <- as_draws_df(fit)
  keep <- grep("^(b_|sd_|sigma$|shape$)", names(draws), value = TRUE)
  distinct <- nrow(unique(as.data.frame(draws)[keep]))
  rows[[length(rows) + 1L]] <<- data.frame(
    model = model, backend = backend, algorithm = algorithm, seed = seed,
    variable = keep,
    mean = vapply(keep, function(v) mean(draws[[v]]), numeric(1)),
    sd = vapply(keep, function(v) sd(draws[[v]]), numeric(1)),
    ndraws = nrow(draws), distinct_draws = distinct,
    row.names = NULL
  )
  notes[[length(notes) + 1L]] <<- data.frame(
    model = model, backend = backend, algorithm = algorithm, seed = seed,
    warnings = paste(unique(warnings), collapse = " | ")
  )
}

run <- function(model, backend, algorithm, seed, ...) {
  spec <- models[[model]]
  warnings <- character()
  fit <- tryCatch(
    withCallingHandlers(
      brm(spec$formula, spec$data, family = spec$family, backend = backend,
          algorithm = algorithm, seed = seed, silent = 2, refresh = 0, ...),
      warning = function(w) {
        warnings <<- c(warnings, conditionMessage(w))
        invokeRestart("muffleWarning")
      }
    ),
    error = function(e) {
      notes[[length(notes) + 1L]] <<- data.frame(
        model = model, backend = backend, algorithm = algorithm, seed = seed,
        warnings = paste("ERROR:", conditionMessage(e))
      )
      NULL
    }
  )
  if (!is.null(fit)) record(fit, model, backend, algorithm, seed, warnings)
  fit
}

for (model in names(models)) {
  message(model)
  run(model, "stanli", "sampling", 1, chains = 4, iter = 2000)
  for (algorithm in algorithms) {
    for (seed in approximation_seeds) run(model, "stanli", algorithm, seed)
  }
  if (with_rstan && model %in% rstan_models) {
    # one compile, reused for both families and every seed
    first <- run(model, "rstan", "meanfield", approximation_seeds[1])
    for (algorithm in c("meanfield", "fullrank")) {
      for (seed in approximation_seeds) {
        if (algorithm == "meanfield" && seed == approximation_seeds[1]) next
        if (is.null(first)) next
        warnings <- character()
        fit <- withCallingHandlers(
          # iter is given because update() would otherwise reuse the number
          # of draws of the first fit as the iteration limit
          update(first, algorithm = algorithm, seed = seed, iter = 2000,
                 silent = 2, refresh = 0, recompile = FALSE),
          warning = function(w) {
            warnings <<- c(warnings, conditionMessage(w))
            invokeRestart("muffleWarning")
          }
        )
        record(fit, model, "rstan", algorithm, seed, warnings)
      }
    }
  }
  write.csv(do.call(rbind, rows), out_file, row.names = FALSE)
  write.csv(do.call(rbind, notes), sub("\\.csv$", "_notes.csv", out_file),
            row.names = FALSE)
}
