functions {
  real widen(real s) {
    return 2 * s;
  }
  void shifted_lp(real y, real s) {
    real wide = 2 * s;
    y ~ normal(1, wide);
  }
  real scaled_lp(real x, real s) {
    real wide = 2 * s;
    x ~ normal(0, s);
    x ~ normal(1, wide);
    return 0;
  }
}
data {
  real<lower=0> s;
}
parameters {
  real tau;
}
model {
  tau ~ normal(0, widen(s));
  shifted_lp(0.25, s);
  if (tau > 0) {
    target += scaled_lp(tau, s);
    target += scaled_lp(0.25, s);
  }
}
