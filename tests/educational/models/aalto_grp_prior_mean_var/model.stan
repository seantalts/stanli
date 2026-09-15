// Hierarchical group means and variances
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 8.3 Unequal variance and hierarchical prior for mean and variance
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
  real mu0;
  real<lower=0> musigma0;
  vector[K] mu;
  real lsigma0;
  real<lower=0> lsigma0s;
  vector<lower=0>[K] sigma;
}
model {
  mu0 ~ normal(10, 10);
  musigma0 ~ normal(0, 10);
  mu ~ normal(mu0, musigma0);
  lsigma0 ~ normal(0, 1);
  lsigma0s ~ lognormal(log(0.1), .5);
  sigma ~ lognormal(lsigma0, lsigma0s);
  y ~ normal(mu[x], sigma[x]);
}
