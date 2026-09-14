// Generalized Pareto with user-defined probability functions
// Extracted from: https://mc-stan.org/learn-stan/case-studies/gpareto_functions.html
// Source location: Stan code with user defined functions; complete gpareto.stan block
// Copyright (c) 2017, Aki Vehtari.
// BSD-3-Clause; see licenses/Gpareto-BSD-3-Clause.txt.
// Browser-rendered source transcription, 2026-09-13.
// Most teaching comments removed; whitespace normalized; executable code preserved.
// Not a byte-for-byte repository download. Not compiler-validated.
functions {
  real gpareto_lpdf(vector y, real ymin, real k, real sigma) {
    int N = rows(y);
    real inv_k = inv(k);
    if (k < 0 && max(y - ymin) / sigma > -inv_k) {
      reject("k<0 and max(y-ymin)/sigma > -1/k; found k, sigma =", k, sigma);
    }
    if (sigma <= 0) {
      reject("sigma<=0; found sigma =", sigma);
    }
    if (abs(k) > 1e-15) {
      return -(1 + inv_k) * sum(log1p((y - ymin) * (k / sigma)))
             - N * log(sigma);
    } else {
      return -sum(y - ymin) / sigma - N * log(sigma);
    }
  }
  real gpareto_cdf(vector y, real ymin, real k, real sigma) {
    real inv_k = inv(k);
    if (k < 0 && max(y - ymin) / sigma > -inv_k) {
      reject("k<0 and max(y-ymin)/sigma > -1/k; found k, sigma =", k, sigma);
    }
    if (sigma <= 0) {
      reject("sigma<=0; found sigma =", sigma);
    }
    if (abs(k) > 1e-15) {
      return exp(sum(log1m_exp((-inv_k) * log1p((y - ymin) * (k / sigma)))));
    } else {
      return exp(sum(log1m_exp(-(y - ymin) / sigma)));
    }
  }
  real gpareto_lcdf(vector y, real ymin, real k, real sigma) {
    real inv_k = inv(k);
    if (k < 0 && max(y - ymin) / sigma > -inv_k) {
      reject("k<0 and max(y-ymin)/sigma > -1/k; found k, sigma =", k, sigma);
    }
    if (sigma <= 0) {
      reject("sigma<=0; found sigma =", sigma);
    }
    if (abs(k) > 1e-15) {
      return sum(log1m_exp((-inv_k) * log1p((y - ymin) * (k / sigma))));
    } else {
      return sum(log1m_exp(-(y - ymin) / sigma));
    }
  }
  real gpareto_lccdf(vector y, real ymin, real k, real sigma) {
    real inv_k = inv(k);
    if (k < 0 && max(y - ymin) / sigma > -inv_k) {
      reject("k<0 and max(y-ymin)/sigma > -1/k; found k, sigma =", k, sigma);
    }
    if (sigma <= 0) {
      reject("sigma<=0; found sigma =", sigma);
    }
    if (abs(k) > 1e-15) {
      return (-inv_k) * sum(log1p((y - ymin) * (k / sigma)));
    } else {
      return -sum(y - ymin) / sigma;
    }
  }
  real gpareto_rng(real ymin, real k, real sigma) {
    if (sigma <= 0) {
      reject("sigma<=0; found sigma =", sigma);
    }
    if (abs(k) > 1e-15) {
      return ymin + (uniform_rng(0, 1) ^ -k - 1) * sigma / k;
    } else {
      return ymin - sigma * log(uniform_rng(0, 1));
    }
  }
}
data {
  real ymin;
  int<lower=0> N;
  vector<lower=ymin>[N] y;
  int<lower=0> Nt;
  vector<lower=ymin>[Nt] yt;
}
transformed data {
  real ymax = max(y);
}
parameters {
  real<lower=0> sigma;
  real<lower=-sigma / (ymax - ymin)> k;
}
model {
  y ~ gpareto(ymin, k, sigma);
}
generated quantities {
  vector[N] log_lik;
  vector[N] yrep;
  vector[Nt] predccdf;
  for (n in 1 : N) {
    log_lik[n] = gpareto_lpdf(rep_vector(y[n], 1) | ymin, k, sigma);
    yrep[n] = gpareto_rng(ymin, k, sigma);
  }
  for (nt in 1 : Nt) {
    predccdf[nt] = exp(gpareto_lccdf(rep_vector(yt[nt], 1) | ymin, k, sigma));
  }
}
