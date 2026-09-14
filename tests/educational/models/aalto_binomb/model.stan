// Binomial on the logit scale
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
// Source location: 3.1 Explicit transformation of variables
// Copyright (c) 2016-, Aki Vehtari, Markus Paasiniemi.
// BSD-3-Clause; see licenses/Aalto-BDA-R-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
data {
  int<lower=0> N;
  int<lower=0> y;
}
parameters {
  real alpha;
}
transformed parameters {
  real theta = inv_logit(alpha);
}
model {
  alpha ~ normal(0, 1.5);
  y ~ binomial_logit(N, alpha);
}
