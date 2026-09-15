// Gaussian linear regression with adjustable priors
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 5.1 Gaussian linear model with adjustable priors
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
  real pmualpha;
  real psalpha;
  real pmubeta;
  real psbeta;
  real pssigma;
}
parameters {
  real alpha;
  real beta;
  real<lower=0> sigma;
}
transformed parameters {
  vector[N] mu = alpha + beta * x;
}
model {
  alpha ~ normal(pmualpha, psalpha);
  beta ~ normal(pmubeta, psbeta);
  sigma ~ normal(0, pssigma);
  y ~ normal(mu, sigma);
}
generated quantities {
  real ypred = normal_rng(alpha + beta * xpred, sigma);
  vector[N] log_lik;
  for (i in 1 : N) {
    log_lik[i] = normal_lpdf(y[i] | mu[i], sigma);
  }
}
