functions {
  real piecewise(real x, real weight) {
    if (x < 1) return x * (weight - 1);
    return x * weight;
  }
}
parameters {
  real theta;
  real weight;
}
model {
  target += piecewise(exp(theta), weight);
}
