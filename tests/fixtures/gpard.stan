parameters {
  array[3] vector[2] x;
  array[3] vector[1] z;
  real<lower=0> alpha;
  array[2] real<lower=0> l;
  array[1] real<lower=0> l1;
  real<lower=0> jitter;
}
model {
  matrix[3, 3] q = gp_exp_quad_cov(x, alpha, l);
  matrix[3, 3] a = gp_matern32_cov(x, alpha, l);
  for (i in 1:3) a[i, i] += jitter;
  matrix[3, 3] b = gp_matern52_cov(x, alpha, l);
  matrix[3, 3] c = gp_exponential_cov(x, alpha, l);
  matrix[3, 3] d = gp_exp_quad_cov(z, alpha, l1);
  target += sum(q) + sum(a) + 2 * sum(b) + 3 * sum(c) + 4 * sum(d);
  target += q[1, 2] * a[1, 2] * b[2, 3] * c[1, 3] * d[2, 3];
}
generated quantities {
  matrix[3, 3] Kq = gp_exp_quad_cov(x, alpha, l);
  matrix[3, 3] K32 = gp_matern32_cov(x, alpha, l);
  matrix[3, 3] K52 = gp_matern52_cov(x, alpha, l);
  matrix[3, 3] Kexp = gp_exponential_cov(x, alpha, l);
  matrix[3, 3] K1 = gp_matern32_cov(z, alpha, l1);
}
