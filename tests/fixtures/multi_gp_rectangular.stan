// multi_gp with more columns than kernel scales: y is K x N, Sigma N x N.
parameters {
  matrix[3, 4] y;
  cov_matrix[4] Sigma;
  cholesky_factor_cov[4] L;
  vector<lower=0>[3] w;
}
model {
  target += multi_gp_lpdf(y | Sigma, w);
  target += multi_gp_cholesky_lpdf(y | L, w);
}
