data {
  int<lower=1> N;
  int<lower=1> J;
  int<lower=1> K;
  matrix[N, K] X;
  array[N] int<lower=1, upper=J> s;
  vector[N] y;
}
parameters {
  vector[K] beta;
  vector[J] a;
  real<lower=0> sigma;
}
model {
  beta ~ normal(0, 1);
  a ~ normal(0, 1);
  sigma ~ exponential(1);
  y ~ normal(X * beta + a[s], sigma);
}
generated quantities {
  vector[N] log_lik;
  for (i in 1 : N) {
    log_lik[i] = normal_lpdf(y[i] | X[i] * beta + a[s[i]], sigma);
  }
}
