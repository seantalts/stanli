functions {
  real scaled_lp(real x, real sigma) {
    x ~ normal(0, sigma);
    return 0;
  }
  real shifted_lpdf(real x, real mu, real sigma) {
    return normal_lupdf(x | mu, sigma);
  }
}
data {
  real<lower=0> s;
}
parameters {
  real tau;
}
model {
  target += scaled_lp(tau, s);
  tau ~ shifted(1, s);
  target += shifted_lpdf(tau | 1, s);
}
