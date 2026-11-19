data {
  int<lower=1> N;
  int<lower=1> K;
  array[N] real y;
  vector[K] w;
}
parameters {
  vector[K] mu;
  vector<lower=0>[K] sigma;
}
model {
  vector[K] ps;
  for (n in 1:N) {
    for (k in 1:K) ps[k] = log(w[k]) + normal_lpdf(y[n] | mu[k], sigma[k]);
    target += log_sum_exp(ps);
  }
  mu ~ normal(0, 5);
  sigma ~ lognormal(0, 1);
}
