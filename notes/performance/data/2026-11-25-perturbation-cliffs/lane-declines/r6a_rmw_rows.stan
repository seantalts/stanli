// Sub-cause 6a: three += statements on mu[n] in one row loop. Each row reads
// mu[n] back from the vector that every row writes.
data {
  int<lower=1> N;
  int<lower=1> J;
  array[N] int<lower=1, upper=J> g;
  vector[N] z;
  matrix[N, 4] X;
  vector[N] y;
}
parameters {
  real c;
  vector[J] r;
  vector[4] b;
  real<lower=0> sigma;
}
model {
  vector[N] mu = rep_vector(0.0, N);
  for (n in 1:N) {
    mu[n] += c;
    mu[n] += r[g[n]] * z[n];
    mu[n] += X[n] * b;
  }
  r ~ normal(0, 1);
  y ~ normal(mu, sigma);
}
