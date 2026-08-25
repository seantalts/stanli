functions {
  matrix cov_matrix_2d(vector sigma, real rho) {
    matrix[2, 2] covariance;
    covariance[1, 1] = square(sigma[1]);
    covariance[1, 2] = sigma[1] * sigma[2] * rho;
    covariance[2, 1] = covariance[1, 2];
    covariance[2, 2] = square(sigma[2]);
    return covariance;
  }
}
parameters {
  vector[2] mu;
  vector<lower=0>[2] sigma;
  real<lower=-1, upper=1> rho;
}
model {
  mu ~ normal(0, 1);
  sigma ~ lognormal(0, 1);
  rho ~ uniform(-1, 1);
}
generated quantities {
  vector[2] draw = multi_normal_rng(mu, cov_matrix_2d(sigma, rho));
  real shifted = draw[1] - draw[2];
  real tail = uniform_rng(0, 1);
}
