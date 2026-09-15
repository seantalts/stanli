// Standardized Gaussian linear regression
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 5.2 Gaussian linear model with standardized data
// Copyright (c) 2016-, Aki Vehtari, Markus Paasiniemi.
// BSD-3-Clause; see licenses/Aalto-BDA-R-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
data {
  int<lower=0> N;
  vector[N] x;
  vector[N] y;
  real xpred;
}
transformed data {
  vector[N] x_std = (x - mean(x)) / sd(x);
  vector[N] y_std = (y - mean(y)) / sd(y);
  real xpred_std = (xpred - mean(x)) / sd(x);
}
parameters {
  real alpha;
  real beta;
  real<lower=0> sigma_std;
}
transformed parameters {
  vector[N] mu_std = alpha + beta * x_std;
}
model {
  alpha ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma_std ~ normal(0, 1);
  y_std ~ normal(mu_std, sigma_std);
}
generated quantities {
  vector[N] mu = mu_std * sd(y) + mean(y);
  real<lower=0> sigma = sigma_std * sd(y);
  real ypred = normal_rng((alpha + beta * xpred_std) * sd(y) + mean(y),
                         sigma_std * sd(y));
  vector[N] log_lik;
  for (i in 1 : N) {
    log_lik[i] = normal_lpdf(y[i] | mu[i], sigma);
  }
}
