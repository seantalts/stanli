// cat.stan with a generated categorical density of a random outcome. The
// runtime outcome cannot become categorical's compile-time integer payload,
// so write_array runs in the MIR interpreter (tests/test_capi.cpp).
data {
  int<lower=1> K;
  int<lower=1, upper=K> y;
  array[3] int<lower=1, upper=K> ys;
}
parameters {
  simplex[K] theta;
}
model {
  target += categorical_lpmf(y | theta);
  target += categorical_lpmf(ys | theta);
}
generated quantities {
  real categorical_value = categorical_lpmf(binomial_rng(1, 1.0) | theta);
}
