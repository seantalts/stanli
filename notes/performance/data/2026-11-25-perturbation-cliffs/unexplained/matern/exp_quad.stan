data { int<lower=1> N; array[N] real x; vector[N] y; }
parameters { real<lower=0> rho; real<lower=0> alpha; real<lower=0> sigma; }
model {
  matrix[N, N] L = cholesky_decompose(add_diag(gp_exp_quad_cov(x, alpha, rho), sigma));
  rho ~ gamma(25, 4); alpha ~ normal(0, 2); sigma ~ normal(0, 1);
  y ~ multi_normal_cholesky(rep_vector(0, N), L);
}
