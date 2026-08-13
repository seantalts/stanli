functions {
  real recursive_value(real x) {
    if (x <= 0) {
      return 0;
    }
    return recursive_value(x - 1);
  }
}
parameters {
  real theta;
}
model {
  target += recursive_value(theta);
}
