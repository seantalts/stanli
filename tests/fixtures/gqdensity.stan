// neg_binomial_2_lpmf / multi_normal_lpdf as value-returning densities in a
// runtime-control region -- WaInterp only.
data {
  int<lower=1> K;
  array[2] int<lower=0> counts;
  vector[K] y;
  matrix[K, K] Sigma;
}
parameters {
  vector[K] mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  real nb = 0;
  real mvn = 0;
  int i = 1;
  while (i <= 2 && mu[1] > -1e9) {
    nb += neg_binomial_2_lpmf(counts[i] | 2.0, 1.5);
    mvn += multi_normal_lpdf(y | mu, Sigma);
    i += 1;
  }
}
