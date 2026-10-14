data {
  int<lower=1> N;
  int<lower=1> K;
  matrix[N, K] x;
  array[N] int y;
}
parameters {
  vector[N] alpha;
  vector[K] beta;
}
model {
  target += poisson_log_glm_lpmf(y | x, alpha, beta);
  target += 0.75 * poisson_log_glm_lpmf(y | x, alpha, beta);
}
