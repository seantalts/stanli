data {
  int<lower=1> N;
  int<lower=1> K;
  array[N] vector[K] y;
  matrix[K, K] Sigma;
  matrix[K, K] precision;
  matrix[K, K] L;
  real<lower=0> nu;
}
parameters {
  array[N] vector[K] mu;
}
model {
  target += multi_normal_lpdf(y | mu, Sigma);
  target += multi_normal_prec_lpdf(y | mu, precision);
  target += multi_normal_cholesky_lpdf(y | mu, L);
  target += multi_student_t_lpdf(y | nu, mu, Sigma);
  target += multi_student_t_cholesky_lpdf(y | nu, mu, L);

  // Exercise the same registry shape policy through a runtime-control
  // ProgramCompiler region as well as ordinary graph lowering.
  if (mu[1][1] > -100) {
    target += multi_normal_lpdf(y | mu, Sigma);
    target += multi_normal_prec_lpdf(y | mu, precision);
    target += multi_normal_cholesky_lpdf(y | mu, L);
    target += multi_student_t_lpdf(y | nu, mu, Sigma);
    target += multi_student_t_cholesky_lpdf(y | nu, mu, L);
  }
}
