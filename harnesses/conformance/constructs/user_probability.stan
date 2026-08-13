functions {
  real custom_lpdf(real y, real mu) {
    return normal_lupdf(y | mu, 1);
  }
}
parameters {
  real theta;
}
model {
  target += custom_lupdf(theta | 0.3);
}
