// `_lupdf` calls in user densities that the model calls as `_lpdf`. Every
// argument is data, and the normalized caller keeps their constants.
functions {
  real local_lpdf(real y) {
    real z = cauchy_lupdf(0.5 | 0.5, 0.5);
    return z;
  }
  real branch_lpdf(real y) {
    real z = cauchy_lupdf(0.5 | 0.5, 0.5);
    if (z < -0.1) {
      return 1;
    }
    return 0;
  }
}
parameters {
  real mu;
}
model {
  target += local_lpdf(mu | );
  target += branch_lpdf(mu | );
  mu ~ normal(0, 1);
}
