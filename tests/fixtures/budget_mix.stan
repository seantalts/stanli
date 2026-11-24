data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real<lower=0, upper=1> theta;
  ordered[2] mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 3);
  sigma ~ exponential(1);
  // Reroll declines this body, so it unrolls to about nine ops a row.
  for (n in 1:N)
    target += log_sum_exp(log(theta) + normal_lpdf(y[n] | mu[1], sigma),
                          log1m(theta) + normal_lpdf(y[n] | mu[2], sigma));
}
