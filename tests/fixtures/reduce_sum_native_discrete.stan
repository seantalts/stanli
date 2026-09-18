functions {
  real partial_lpmf(array[] int y, int first, int last, real b) {
    return poisson_log_lupmf(y | b);
  }
  real announced(real b) {
    print("shared argument");
    return b;
  }
}
data {
  int<lower=0> N;
  array[N] int<lower=0> y;
}
parameters { real b; }
model {
  target += -0.5 * square(b);
  target += reduce_sum(partial_lpmf, y, 1, announced(b));
  target += reduce_sum_static(partial_lupmf, y, 3, b);
}
