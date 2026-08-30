functions {
  real dynamic_declared_nan_read(real theta) {
    int k = 1;
    vector[2] x;
    real out;
    if (theta > 0)
      k = 2;
    if (is_nan(x[k]))
      out = theta;
    else
      out = 2 * theta;
    return out;
  }
}

parameters {
  real theta;
}

model {
  target += dynamic_declared_nan_read(theta);
}
