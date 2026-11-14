functions {
  real scaled_lp(real x, real s) {
    real wide = 2 * s;
    x ~ normal(0, s);
    x ~ normal(1, wide);
    return 0;
  }
  void centered_lp(real x, real s) {
    if (x > 1) return;
    x ~ normal(0.5, s);
  }
  void guarded_lp(real x, real s) {
    if (x > 0) x ~ normal(-0.5, s);
  }
  real shifted_lpdf(real x, real mu, real s) {
    return normal_lupdf(x | mu, s);
  }
}
data {
  real<lower=0> s;
}
parameters {
  real tau;
}
model {
  guarded_lp(tau, s);
  if (tau > 0) {
    target += scaled_lp(tau, s);
    target += scaled_lp(0.25, s);
    centered_lp(tau, s);
    target += shifted_lpdf(tau | 1, s);
    tau ~ shifted(2, s);
  }
}
