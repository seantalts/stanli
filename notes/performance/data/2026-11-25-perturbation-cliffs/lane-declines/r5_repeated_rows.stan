// Sub-cause 5: the same program as radon_variable_intercept_slope_noncentered's
// likelihood. Whether partition fuses the rows depends only on how many rows
// are distinct (see r5_make_data.py).
data {
  int<lower=1> N;
  int<lower=1> J;
  array[N] int<lower=1, upper=J> g;
  vector[N] x;
  vector[N] y;
}
parameters {
  vector[J] a_raw;
  vector[J] b_raw;
  real mu_b;
  real<lower=0> sigma;
}
model {
  vector[J] a = 2 * a_raw;
  vector[J] b = mu_b + b_raw;
  a_raw ~ normal(0, 1);
  b_raw ~ normal(0, 1);
  vector[N] mu;
  for (n in 1:N) {
    mu[n] = a[g[n]] + x[n] * b[g[n]];
    target += normal_lpdf(y[n] | mu[n], sigma);
  }
}
