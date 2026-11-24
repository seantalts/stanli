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
  // Large terms of both signs that nearly cancel, so the order in which
  // the target adds them shows in the last bits.
  for (n in 1:N)
    target += log_sum_exp(log(theta) + normal_lpdf(y[n] | mu[1], sigma),
                          log1m(theta) + normal_lpdf(y[n] | mu[2], sigma))
              + 1e6 * x[n] * theta;
}
