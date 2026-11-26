// Sub-cause 1, second gate: the container is a matrix filled up front, and
// each row is reduced with log_sum_exp. Partition has no row rule for it.
data {
  int<lower=0> N;
  array[N] real y;
}
parameters {
  real<lower=0, upper=1> theta;
  array[2] real mu;
}
model {
  vector[2] log_w = [log(theta), log1m(theta)]';
  matrix[N, 2] lp;
  mu ~ normal(0, 10);
  for (k in 1:2)
    for (n in 1:N)
      lp[n, k] = log_w[k] + normal_lpdf(y[n] | mu[k], 1);
  for (n in 1:N) target += log_sum_exp(lp[n]);
}
