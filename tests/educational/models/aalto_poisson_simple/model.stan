// Poisson count model with posterior predictive replication
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/ppc/poisson-ppc.html
// Source location: 2 Fit basic Poisson model
// Authors: Jonah Gabry and Aki Vehtari.
// Copyright (c) 2016-, Aki Vehtari, Markus Paasiniemi (repository notice).
// BSD-3-Clause; see licenses/Aalto-BDA-R-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
data {
  int<lower=1> N;
  array[N] int<lower=0> y;
}
parameters {
  real<lower=0> lambda;
}
model {
  lambda ~ exponential(0.2);
  y ~ poisson(lambda);
}
generated quantities {
  array[N] real log_lik;
  array[N] int y_rep;
  for (n in 1 : N) {
    y_rep[n] = poisson_rng(lambda);
    log_lik[n] = poisson_lpmf(y[n] | lambda);
  }
}
