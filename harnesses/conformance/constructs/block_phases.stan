data {
  real x;
}
transformed data {
  real twice_x = 2 * x;
}
parameters {
  real theta;
}
transformed parameters {
  real shifted = theta + twice_x;
}
model {
  target += normal_lpdf(shifted | 0, 1);
}
generated quantities {
  real reported = shifted + x;
}
