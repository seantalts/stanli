data {
  int<lower=1> T;
  int<lower=1> K;
  array[T] real y;
  array[K] vector[K] log_gamma;
}
parameters {
  vector[K] mu;
  real<lower=0> sigma;
}
model {
  vector[K] alpha = rep_vector(-log(K), K);
  vector[K] next;
  for (t in 1:T) {
    for (k in 1:K) {
      vector[K] acc;
      for (j in 1:K) acc[j] = alpha[j] + log_gamma[j, k];
      next[k] = log_sum_exp(acc) + normal_lpdf(y[t] | mu[k], sigma);
    }
    alpha = next;
  }
  target += log_sum_exp(alpha);
  mu ~ normal(0, 5);
  sigma ~ lognormal(0, 1);
}
