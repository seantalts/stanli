functions {
  void check_nonnegative(real x) {
    if (x < 0) reject("x must be nonnegative: ", x);
  }
  void prior_lp(vector theta, vector mu, real sigma, int skip) {
    if (skip) return;
    for (n in 1:rows(theta)) theta[n] ~ normal(mu[n], sigma);
  }
  void weighted_lp(real x, int k) {
    if (k == 2) return;
    target += k * x;
  }
  void scaled_lp(real x, real w) {
    target += w * normal_lpdf(x | 0, 2);
    prior_lp(rep_vector(x, 1), rep_vector(0, 1), 3, 0);
  }
}
data {
  int N;
  vector[N] mu;
  real<lower=0> sigma;
  int skip;
}
parameters {
  vector[N] theta;
  real tau;
}
transformed parameters {
  real tau2 = square(tau);
  check_nonnegative(tau2);
}
model {
  prior_lp(theta, mu, sigma, skip);
  prior_lp(theta, mu, sigma, 1);
  for (n in 1:N) weighted_lp(theta[n], n);
  scaled_lp(tau, 1.5);
}
generated quantities {
  real shifted = tau2 + 1;
  check_nonnegative(shifted);
}
