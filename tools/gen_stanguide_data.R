#!/usr/bin/env Rscript
# Writes the data files of tests/stanguide. The Stan User's Guide gives data
# declarations but no data for these programs, so every file here is
# simulated or derived by this script. Each section says how.
#
#   changepoint         The guide cites Jarrett (1979) for its coal mining
#                       disaster counts of 1851 to 1962 but prints no numbers.
#                       D is the number of explosions per calendar year in
#                       boot::coal (boot 1.3-32, whose License field says
#                       "Unlimited"): 191 explosions in 112 years. The prior
#                       rates r_e and r_l are not specified by the guide; both
#                       are 1 / mean(D).
#   cjs_history         Counts of the profiles 2 to 7 of the guide's table,
#                       400 captured animals drawn with rmultinom from the
#                       profile probabilities at phi = (0.8, 0.7) and
#                       p = (_, 0.6, 0.5), conditioned on being captured at
#                       least once before the last occasion. history[1] (the
#                       profile that carries no information) is 0.
#   map_rect_logistic   12 observations, x ~ normal(0, 1), logistic
#                       regression with intercept -0.3 and slope 1.2.
#   integrate_1d_normal 20 observations from normal(0.5, 1.2).
#   ode_adjoint         10 observations of the damped harmonic oscillator of
#                       the guide, y'' = -y - theta y' with theta = 0.15 and
#                       y(0) = (1, 0), at t = 1 .. 10 plus normal(0, 0.1)
#                       noise. The solution is the matrix exponential of the
#                       system matrix.
#   copula              50 draws of 3 dimensions from a Gaussian copula with
#                       exponential marginals (rates 1, 2, 0.5) and
#                       correlations 0.6, 0.3, -0.2.
#
# The declaration shapes come from the checked-in .stan files, and
# set.seed(1) starts every section.
#
# Usage:
#   Rscript tools/gen_stanguide_data.R [outdir]   # default tests/stanguide

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/stanguide"

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

emit_model <- function(id, data) {
  stan <- file.path(OUT, paste0("stanguide_", id, ".stan"))
  code <- paste(readLines(stan), collapse = "\n")
  write_json(data, code, file.path(OUT, paste0("stanguide_", id, ".json")))
}

stopifnot(requireNamespace("boot", quietly = TRUE))
D <- tabulate(floor(boot::coal$date) - 1850L, nbins = 112L)
stopifnot(sum(D) == 191L)
emit_model("changepoint",
           list(r_e = 1 / mean(D), r_l = 1 / mean(D), T = length(D),
                D = as.integer(D)))

set.seed(1)
phi <- c(0.8, 0.7); p <- c(NA, 0.6, 0.5)
chi2 <- (1 - phi[2]) + phi[2] * (1 - p[3])
chi1 <- (1 - phi[1]) + phi[1] * (1 - p[2]) * chi2
prob <- c(chi2, phi[2] * p[3], chi1,
          phi[1] * (1 - p[2]) * phi[2] * p[3], phi[1] * p[2] * chi2,
          phi[1] * p[2] * phi[2] * p[3])
history <- c(0L, as.integer(rmultinom(1, 400, prob / sum(prob))))
emit_model("cjs_history", list(history = history))

set.seed(1)
x <- rnorm(12)
y <- rbinom(12, 1, plogis(-0.3 + 1.2 * x))
emit_model("map_rect_logistic", list(y = y, x = x))

set.seed(1)
emit_model("integrate_1d_normal", list(N = 20L, y = rnorm(20, 0.5, 1.2)))

set.seed(1)
theta <- 0.15
A <- matrix(c(0, -1, 1, -theta), 2, 2)
ev <- eigen(A)
ts <- as.numeric(1:10)
y <- t(sapply(ts, function(t)
  Re(ev$vectors %*% (exp(ev$values * t) * solve(ev$vectors, c(1, 0))))))
y <- y + matrix(rnorm(length(y), 0, 0.1), nrow(y))
emit_model("ode_adjoint",
           list(T = 10L, y = lapply(seq_len(nrow(y)), function(i) y[i, ]),
                t0 = 0, ts = ts))

set.seed(1)
N <- 50L; Dm <- 3L
R <- matrix(c(1, 0.6, 0.3, 0.6, 1, -0.2, 0.3, -0.2, 1), 3, 3)
z <- matrix(rnorm(N * Dm), N, Dm) %*% chol(R)
rate <- c(1, 2, 0.5)
yc <- t(apply(pnorm(z), 1, function(u) qexp(u, rate)))
emit_model("copula",
           list(N = N, D = Dm, y = lapply(seq_len(N), function(i) yc[i, ])))
