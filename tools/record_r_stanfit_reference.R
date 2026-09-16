# Developer-only oracle generation: requires RStan and a C++ toolchain.
# The package, its tests, and end-user conversion never compile a C++ model.
# Run from the repository root. This script never loads stanli.
suppressPackageStartupMessages(library(rstan))
code <- paste(readLines("r/tests/testthat/fixtures/stanfit-live.stan"), collapse = "\n")
model <- rstan::stan_model(model_code = code, model_name = "stanli_live_oracle")
fit <- rstan::sampling(model, chains = 1, iter = 1, warmup = 0,
                       algorithm = "Fixed_param", init = 0, seed = 731, refresh = 0)
n <- rstan::get_num_upars(fit)
points <- lapply(list(rep(0, n), seq(-0.4, 0.4, length.out = n),
                     seq(0.7, -0.2, length.out = n)), function(q) {
  constrained <- rstan::constrain_pars(fit, q)
  list(q = q, lp = rstan::log_prob(fit, q, gradient = TRUE),
       grad = rstan::grad_log_prob(fit, q),
       without_jacobian = rstan::log_prob(fit, q, adjust_transform = FALSE,
                                         gradient = TRUE),
       constrained = constrained,
       unconstrained = rstan::unconstrain_pars(fit, constrained))
})
reference <- list(rstan = as.character(packageVersion("rstan")),
                  StanHeaders = as.character(packageVersion("StanHeaders")),
                  stan = rstan::stan_version(), code = code, n = n, points = points)
output <- capture.output(dput(reference))
writeLines(sub("[[:blank:]]+$", "", output),
           "r/tests/testthat/fixtures/stanfit-live-reference.R")
