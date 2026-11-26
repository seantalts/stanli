// Sub-cause 2: one active scalar (sigma) is read at two positions per row.
data {
  int<lower=0> N;
  array[N] real y;
}
parameters {
  real<lower=0, upper=1> theta;
  array[2] real mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 10);
  sigma ~ normal(0, 2);
  for (n in 1:N)
    target += log_mix(theta, normal_lpdf(y[n] | mu[1], sigma),
                      normal_lpdf(y[n] | mu[2], sigma));
}
