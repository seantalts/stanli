// Poisson hurdle model with upper truncation
// Extracted from: https://avehtari.github.io/BDA_R_demos/demos_rstan/ppc/poisson-ppc.html
// Source location: 4 Fit Poisson hurdle model (also with truncation from above)
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
transformed data {
  int U = max(y);
}
parameters {
  real<lower=0, upper=1> theta;
  real<lower=0> lambda;
}
model {
  lambda ~ exponential(0.2);
  for (n in 1 : N) {
    if (y[n] == 0) {
      target += log(theta);
    } else {
      target += log1m(theta);
      y[n] ~ poisson(lambda) T[1, U];
    }
  }
}
generated quantities {
  array[N] real log_lik;
  array[N] int y_rep;
  for (n in 1 : N) {
    if (bernoulli_rng(theta)) {
      y_rep[n] = 0;
    } else {
      int w;
      w = poisson_rng(lambda);
      while (w == 0 || w > U) {
        w = poisson_rng(lambda);
      }
      y_rep[n] = w;
    }
    if (y[n] == 0) {
      log_lik[n] = log(theta);
    } else {
      log_lik[n] = log1m(theta) + poisson_lpmf(y[n] | lambda)
                   - log_diff_exp(poisson_lcdf(U | lambda),
                                  poisson_lcdf(0 | lambda));
    }
  }
}
