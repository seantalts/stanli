#!/usr/bin/env Rscript
# Writes the data files in tests/examplemodels that the upstream repository
# does not ship as JSON: the two models whose data example-models only
# describes in prose or in a notebook.
#
#   examplemodels_hmm_marginal.json
#     knitr/hmm-example/hmm-example.Rmd simulates its data in two R chunks.
#     This script runs the same statements, with set.seed(1) added.
#   examplemodels_sum_to_zero_binomial.json
#     jupyter/sum-to-zero/stan/gen_binomial_4_preds.stan is a Stan program
#     that simulates the data when run with fixed_param. This script ports
#     it to R statement for statement, with the notebook's arguments
#     (N_eth = 3, N_edu = 5, N_age = 9, baseline = -3.5, sens = 0.75,
#     spec = 0.9995, 17 tests per stratum on average) and set.seed(56789).
#     R's generators replace Stan's, so the values differ from the notebook's
#     small data set; the model is the same.
#
#   examplemodels_irt_1pl_hier.json
#     knitr/irt/irt_1pl-sim.R simulates a 10 item by 20 student test. This
#     script runs the same statements with set.seed(1).
#   examplemodels_irt_multilevel.json
#     No upstream script simulates data for irt-multilevel.stan. This script
#     draws 8 items, 30 students in 4 schools and every student by item
#     response from the model's own generative story, with set.seed(1).
#
# Each model's declaration shapes come from the checked-in .stan file. The
# other models in the directory use the JSON files that upstream ships,
# copied unchanged.
#
# Usage:
#   Rscript tools/gen_examplemodels_data.R [outdir]   # default tests/examplemodels

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/examplemodels"

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

emit_model <- function(id, data) {
  stan <- file.path(OUT, paste0("examplemodels_", id, ".stan"))
  code <- paste(readLines(stan), collapse = "\n")
  write_json(data, code, file.path(OUT, paste0("examplemodels_", id, ".json")))
}

set.seed(1)
N <- 100L
states <- rep(1L, N)
for (n in 2:N) {
  if (states[n - 1] == 1) {
    states[n] <- sample(c(1L, 2L), size = 1, prob = c(0.5, 0.5))
  } else if (states[n - 1] == 2) {
    states[n] <- sample(c(1L, 2L, 3L), size = 1, prob = c(0.25, 0.5, 0.25))
  } else if (states[n - 1] == 3) {
    states[n] <- sample(c(2L, 3L), size = 1, prob = c(0.5, 0.5))
  }
}
mus <- c(1.0, 5.0, 9.0)
sigma <- 2.0
y <- rnorm(N, mus[states], sd = sigma)
emit_model("hmm_marginal", list(N = N, y = y))

set.seed(56789)
rdirichlet <- function(alpha) {
  g <- rgamma(length(alpha), shape = alpha)
  g / sum(g)
}
N_eth <- 3L; N_edu <- 5L; N_age <- 9L
baseline <- -3.5; sens <- 0.75; spec <- 0.9995
N_avg <- 17L
strata <- 2L * N_age * N_eth * N_edu
N <- as.integer(N_avg * strata)

beta_sex <- rnorm(1, 0, 0.5)
pct_sex <- c(0.4, 0.6)
pct_age <- rdirichlet(rep(2, N_age));   beta_age <- rnorm(N_age)
pct_eth <- rdirichlet(rep(1, N_eth));   beta_eth <- rnorm(N_eth)
pct_edu <- rdirichlet(rep(1.5, N_edu)); beta_edu <- rnorm(N_edu)

sex <- age <- eth <- edu <- pos_tests <- tests <- integer(strata)
idx <- 1L
for (i_sex in 1:2) for (i_age in 1:N_age) for (i_eth in 1:N_eth) for (i_edu in 1:N_edu) {
  sex[idx] <- i_sex; age[idx] <- i_age; eth[idx] <- i_eth; edu[idx] <- i_edu
  tests[idx] <- as.integer(pct_sex[i_sex] * pct_age[i_age] * pct_eth[i_eth] *
                           pct_edu[i_edu] * N)
  p <- plogis(baseline + beta_sex * i_sex + beta_age[i_age] + beta_eth[i_eth] +
              beta_edu[i_edu])
  p_sample <- p * sens + (1 - p) * (1 - spec)
  pos_tests[idx] <- rbinom(1, tests[idx], p_sample)
  idx <- idx + 1L
}
emit_model("sum_to_zero_binomial",
           list(N = strata, N_age = N_age, N_eth = N_eth, N_edu = N_edu,
                pos_tests = pos_tests, tests = tests, sex = sex, age = age,
                eth = eth, edu = edu, sens = sens, spec = spec))

set.seed(1)
inv_logit <- function(u) 1 / (1 + exp(-u))
I <- 10L; J <- 20L
theta <- rnorm(J, 0, 1)
b <- rnorm(I, -1, 2)
y <- matrix(I * J, I, J)
for (i in 1:I) for (j in 1:J) y[i, j] <- rbinom(1, 1, inv_logit(theta[j] - b[i]))
storage.mode(y) <- "integer"
emit_model("irt_1pl_hier", list(I = I, J = J, y = y))

set.seed(1)
I <- 8L; J <- 30L; K <- 4L
school <- sample(rep(seq_len(K), length.out = J))
gamma <- rnorm(K, 0, 0.5)
alpha <- rnorm(J, 0, 1)
beta <- rnorm(I, 0, 1)
delta <- rlnorm(I, 0, 0.3)
N <- I * J
ii <- rep(seq_len(I), times = J)
jj <- rep(seq_len(J), each = I)
kk <- school[jj]
y <- rbinom(N, 1, inv_logit(delta[ii] * (alpha[jj] + gamma[kk] - beta[ii])))
emit_model("irt_multilevel",
           list(I = I, J = J, K = K, N = N, ii = ii, jj = jj, kk = kk,
                y = as.integer(y)))
