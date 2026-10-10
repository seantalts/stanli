# Two checks of stanli's ADVI against rstan::vb() under one seed, each of
# which compiles one model with rstan (about a minute and a few GB).
#
#   Rscript tools/check_advi_against_rstan.R
#
# rstan and stanli run the same Stan service on the same generator, so with
# one seed they should take the same path. This prints, side by side:
#
# 1. A run that fails. brms's `y ~ x + (1 | g)` with mean-field ADVI and
#    seed 3 diverges; the step size chosen, the lower-bound trace and the
#    exception Stan throws should be the same in both. (rstan::vb() then
#    stops on an error of its own while reading the unfinished output.)
# 2. A model written with `~` statements. stanli's log density leaves their
#    constants out, so its lower bound differs from Stan's by a constant and
#    the relative stopping rule can end on another iteration. Where the two
#    stop on the same iteration the draws should agree to the precision of
#    rstan's output file.
#
# Needs brms (for the Stan code of the first model), rstan, the stanli R
# package and STANLI_RUNTIME.

suppressMessages({library(brms); library(stanli); library(rstan)})

trace <- function(lines) {
  lines <- sub("^Chain 1: ", "", lines)
  keep <- grepl("Found best value|^ +[0-9]+ +-?[0-9.]+ |dropped evaluations",
                lines)
  trimws(lines[keep])
}

cat("== 1. a diverging run: y ~ x + (1 | g), meanfield, seed 3 ==\n")
set.seed(7)
N <- 40
sim <- data.frame(x = rnorm(N), z = rnorm(N), g = factor(rep(1:5, each = 8)),
                  tt = rep(1:8, 5), y = rnorm(N))
code <- make_stancode(y ~ x + (1 | g), sim, family = gaussian())
sdata <- make_standata(y ~ x + (1 | g), sim, family = gaussian())
class(sdata) <- "list"
ours <- capture.output(
  tryCatch(variational_model(stanli_model(code = code, data = sdata, seed = 3),
                             "meanfield", seed = 3, iter = 2000, refresh = 1),
           error = function(e) cat(conditionMessage(e), "\n")))
compiled <- stan_model(model_code = code)
theirs <- capture.output(
  tryCatch(vb(compiled, data = sdata, seed = 3, algorithm = "meanfield",
              iter = 2000, refresh = 1),
           error = function(e) cat("rstan::vb error:", conditionMessage(e), "\n")))
cat("stanli:\n", paste0("  ", trace(ours), "\n"), sep = "")
cat("rstan:\n", paste0("  ", trace(theirs), "\n"), sep = "")

cat("\n== 2. a model written with ~ statements, meanfield, seeds 1 to 5 ==\n")
tilde <- "
data { int N; vector[N] y; }
parameters { real mu; real<lower=0> sigma; }
model { y ~ normal(mu, sigma); mu ~ normal(0, 5); sigma ~ exponential(1); }"
set.seed(3)
dat <- list(N = 30L, y = rnorm(30, 1, 2))
model <- stanli_model(code = tilde, data = dat)
compiled <- stan_model(model_code = tilde)
last <- function(lines) {
  rows <- grep("^ +[0-9]+ +-?[0-9.]+ ", sub("^Chain 1: ", "", lines),
               value = TRUE)
  trimws(sub("^Chain 1: ", "", rows[length(rows)]))
}
for (seed in 1:5) {
  a <- capture.output(f <- variational_model(model, seed = seed, refresh = 1))
  b <- capture.output(g <- suppressWarnings(
    vb(compiled, data = dat, seed = seed, refresh = 1)))
  e <- rstan::extract(g)
  cat(sprintf(paste0(
    "seed %d\n  stanli last line: %s\n  rstan  last line: %s\n",
    "  mu mean (sd): stanli %.5f (%.5f), rstan %.5f (%.5f)\n"),
    seed, last(a), last(b), mean(f$draws[, 1, "mu"]), sd(f$draws[, 1, "mu"]),
    mean(e$mu), sd(e$mu)))
}
