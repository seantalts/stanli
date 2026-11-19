data {
  int<lower=1> T;
  int<lower=1> K;
  array[T] real y;
  array[T] int<lower=1, upper=K> group;
  array[K] vector[K] log_gamma;
}
parameters {
  vector[K] mu;
  real<lower=0> sigma;
}
model {
  vector[K] alpha = rep_vector(-log(K), K);
  vector[K] next;
  real total = 0;
  for (t in 1:T) {
    for (k in 1:K) {
      vector[K] acc;
      for (j in 1:K) acc[j] = alpha[j] + log_gamma[j, k];
      next[k] = log_sum_exp(acc) + normal_lpdf(y[t] | mu[k], sigma);
    }
    alpha = next;
    total += y[t];
    target += 1e-3 * sum(alpha[2:group[t]]);
  }
  target += log_sum_exp(alpha);
  mu ~ normal(total / T, 5);
  sigma ~ lognormal(0, 1);
}
