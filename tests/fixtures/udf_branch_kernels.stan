functions {
  real part_lpdf(array[] real slice, int start, int end, real mu, real s) {
    return normal_lupdf(slice | mu, s);
  }
  real drift_lpdf(real y, real a, real t, real b, real d) {
    return wiener_lupdf(y | a, t, b, d);
  }
}
data {
  real<lower=0> s;
}
parameters {
  real tau;
  array[2] real y;
}
model {
  tau ~ normal(0, 1);
  if (tau > 0) {
    target += reduce_sum(part_lpdf, y, 1, tau, s);
    target += reduce_sum(part_lupdf, y, 1, tau, s);
    target += drift_lupdf(1.0 | 2.0, 0.3, 0.5, 0.2);
    target += drift_lupdf(1 + exp(tau) | 2.0, 0.3, 0.5, 0.2);
  }
}
