// Bernoulli with generated log likelihood
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 2 Bernoulli model
// Copyright (c) 2016-, Aki Vehtari, Markus Paasiniemi.
// BSD-3-Clause; see licenses/Aalto-BDA-R-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
data {
  int<lower=0> N;
  array[N] int<lower=0, upper=1> y;
}
parameters {
  real<lower=0, upper=1> theta;
}
model {
  theta ~ beta(1, 1);
  y ~ bernoulli(theta);
}
generated quantities {
  vector[N] log_lik;
  real lprior = beta_lpdf(theta | 1, 1);
  for (i in 1:N) {
    log_lik[i] = bernoulli_lpmf(y[i] | theta);
  }
}
