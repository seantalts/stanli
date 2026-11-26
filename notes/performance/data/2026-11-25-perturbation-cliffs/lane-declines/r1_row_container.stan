// Sub-cause 1: a per-row local container that is filled element by element
// and then reduced as a whole. Each unrolled row gets its own slot.
data {
  int<lower=0> N;
  array[N] real y;
}
parameters {
  real<lower=0, upper=1> theta;
  array[2] real mu;
}
model {
  mu ~ normal(0, 10);
  for (n in 1:N) {
    vector[2] lp;
    lp[1] = log(theta) + normal_lpdf(y[n] | mu[1], 1);
    lp[2] = log1m(theta) + normal_lpdf(y[n] | mu[2], 1);
    target += log_sum_exp(lp);
  }
}
