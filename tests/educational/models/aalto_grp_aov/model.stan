// Group means with shared variance (ANOVA)
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 8.1 Common variance (ANOVA) model
// Copyright (c) 2016-, Aki Vehtari, Markus Paasiniemi.
// BSD-3-Clause; see licenses/Aalto-BDA-R-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
data {
  int<lower=0> N;
  int<lower=0> K;
  array[N] int<lower=1, upper=K> x;
  vector[N] y;
}
parameters {
  vector[K] mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 100);
  sigma ~ normal(0, 1);
  y ~ normal(mu[x], sigma);
}
