// gp_periodic_cov(x, sigma, l, p) over array[N] real and array[N] vector[D]
// coordinates, with data and parameter periods and two forms of jitter.
transformed data {
  matrix[2, 2] K0 = gp_periodic_cov({1, 2}, 1., 1., 1.);
  array[4] real xs = {0.1, 0.9, 1.7, 3.2};
  array[3] vector[2] xv = {[0.2, -0.4]', [1.1, 0.3]', [-0.5, 0.8]'};
  matrix[4, 4] Kd = gp_periodic_cov(xs, 1.3, 0.8, 2.7);
  vector[4] y = [0.3, -0.2, 0.5, 0.1]';
}
parameters {
  real<lower=0> sigma;
  real<lower=0> l;
  real<lower=0> p;
  real<lower=0> tau;
  array[3] vector[2] z;
}
model {
  matrix[4, 4] K = gp_periodic_cov(xs, sigma, l, p);
  matrix[4, 4] Kl = gp_periodic_cov(xs, sigma, l, 2.7);
  matrix[4, 4] Kp = gp_periodic_cov(xs, 1.3, 0.8, p);
  matrix[3, 3] Kv = gp_periodic_cov(xv, sigma, l, p);
  matrix[3, 3] Kz = gp_periodic_cov(z, sigma, l, p);
  matrix[4, 4] L = gp_periodic_cov(xs, sigma, l, p);
  for (i in 1:4) L[i, i] += tau;
  target += sum(K0) + sum(Kd) + sum(K) + 2 * sum(Kl) + 3 * sum(Kp);
  target += 4 * sum(Kv) + 5 * sum(Kz) + K[1, 2] * Kv[2, 3] * Kz[1, 3];
  target += multi_normal_lpdf(y | rep_vector(0, 4), add_diag(K, 0.3));
  target += multi_normal_lpdf(y | rep_vector(0, 4), L);
}
generated quantities {
  matrix[4, 4] Kq = gp_periodic_cov(xs, sigma, l, p);
  matrix[3, 3] Kzq = gp_periodic_cov(z, sigma, l, p);
  matrix[4, 4] Lq = add_diag(Kq, tau);
}
