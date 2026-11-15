functions {
  void prior_lp(vector theta, real sigma) {
    for (n in 1:rows(theta)) theta[n] ~ normal(0, sigma);
  }
  real row_total(row_vector v) {
    real total = 0;
    for (n in 1:cols(v)) total += v[n];
    return total;
  }
}
parameters {
  real tau;
}
model {
  prior_lp(rep_vector(tau, 1), 3);
  prior_lp(rep_vector(tau, 2), 2);
  target += sum(rep_row_vector(tau, 3)[1:cols(rep_row_vector(tau, 3))]);
  target += sum(rep_matrix(tau, 2, 3)[1:rows(rep_matrix(tau, 2, 3)), 2]);
}
generated quantities {
  real shifted = row_total(rep_row_vector(tau + 1, 2));
}
