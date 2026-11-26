// The dense ops that STANLI_ACCELERATE can give an accelerated kernel, at
// sizes the test chooses through the data: a Gaussian process
// (cholesky_decompose, multi_normal_cholesky with data y), a matrix product
// with a parameter matrix and a matrix-vector product with data.
data {
  int<lower=1> N;
  int<lower=1> K;
  int<lower=1> M;
  int<lower=1> P;
  array[N] real x;
  vector[N] y;
  matrix[N, N] X;
  matrix[M, K] Z;
}
parameters {
  real<lower=0> rho;
  real<lower=0> alpha;
  real<lower=0> sigma;
  matrix[N, P] B;
  vector[K] w;
}
model {
  matrix[N, N] L = cholesky_decompose(
      add_diag(gp_exp_quad_cov(x, alpha, rho), square(sigma)));
  y ~ multi_normal_cholesky(rep_vector(0, N), L);
  to_vector(B) ~ std_normal();
  w ~ std_normal();
  target += -0.5 * dot_self(to_vector(X * B));
  target += -0.5 * dot_self(Z * w);
}
