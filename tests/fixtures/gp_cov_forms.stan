// Every square and cross-covariance signature of the five GP kernels, with
// data and parameter locations. The cross forms use more rows than columns
// and asymmetric points, so a transposed result changes the value.
transformed data {
  array[3] vector[2] x1 = {[0, 0.5]', [1, -0.3]', [2.5, 1.1]'};
  array[2] vector[2] x2 = {[0.2, 0.1]', [-1.4, 0.9]'};
  array[3] real r1 = {0, 1.3, 2.2};
  array[2] real r2 = {0.4, -0.7};
}
parameters {
  real<lower=0> sigma;
  real<lower=0> l;
  array[2] real<lower=0> ld;
  real<lower=0> p;
  array[2] vector[2] xp;
  array[2] real rp;
}
model {
  matrix[3, 2] w;
  matrix[3, 3] ws;
  for (j in 1:2) for (i in 1:3) w[i, j] = 1 + 0.1 * i + 0.37 * j;
  for (j in 1:3) for (i in 1:3) ws[i, j] = 1 + 0.2 * i + 0.11 * j;
  target += sum(w .* gp_exp_quad_cov(x1, x2, sigma, l));
  target += sum(w .* gp_exp_quad_cov(x1, x2, sigma, ld));
  target += sum(w .* gp_exp_quad_cov(r1, r2, sigma, l));
  target += sum(w .* gp_matern32_cov(x1, x2, sigma, l));
  target += sum(w .* gp_matern32_cov(x1, x2, sigma, ld));
  target += sum(w .* gp_matern32_cov(r1, r2, sigma, l));
  target += sum(w .* gp_matern52_cov(x1, x2, sigma, l));
  target += sum(w .* gp_matern52_cov(x1, x2, sigma, ld));
  target += sum(w .* gp_matern52_cov(r1, r2, sigma, l));
  target += sum(w .* gp_exponential_cov(x1, x2, sigma, l));
  target += sum(w .* gp_exponential_cov(x1, x2, sigma, ld));
  target += sum(w .* gp_exponential_cov(r1, r2, sigma, l));
  target += sum(w .* gp_periodic_cov(x1, x2, sigma, l, p));
  target += sum(w .* gp_periodic_cov(r1, r2, sigma, l, p));
  target += sum(w .* gp_exp_quad_cov(x1, xp, sigma, ld));
  target += sum(w .* gp_matern32_cov(r1, rp, sigma, l));
  target += sum(w .* gp_periodic_cov(x1, xp, sigma, l, p));

  target += sum(ws .* gp_exp_quad_cov(x1, sigma, l));
  target += sum(ws .* gp_exp_quad_cov(x1, sigma, ld));
  target += sum(ws .* gp_exp_quad_cov(r1, sigma, l));
  target += sum(ws .* gp_matern32_cov(x1, sigma, l));
  target += sum(ws .* gp_matern32_cov(x1, sigma, ld));
  target += sum(ws .* gp_matern32_cov(r1, sigma, l));
  target += sum(ws .* gp_matern52_cov(x1, sigma, l));
  target += sum(ws .* gp_matern52_cov(x1, sigma, ld));
  target += sum(ws .* gp_matern52_cov(r1, sigma, l));
  target += sum(ws .* gp_exponential_cov(x1, sigma, l));
  target += sum(ws .* gp_exponential_cov(x1, sigma, ld));
  target += sum(ws .* gp_exponential_cov(r1, sigma, l));
  target += sum(ws .* gp_periodic_cov(x1, sigma, l, p));
  target += sum(ws .* gp_periodic_cov(r1, sigma, l, p));

  sigma ~ normal(1, 1);
  l ~ normal(1, 1);
  ld ~ normal(1, 1);
  p ~ normal(2, 1);
  for (i in 1:2) xp[i] ~ normal(0, 1);
  rp ~ normal(0, 1);
}
generated quantities {
  matrix[3, 2] k_cross = gp_matern52_cov(x1, xp, sigma, ld);
  matrix[3, 2] k_real = gp_exponential_cov(r1, rp, sigma, l);
}
