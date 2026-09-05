transformed data {
  array[3] int order = {0, 1, 2};
  array[3] int label = {0, 1, 0};
  array[3] int count = {0, 1, 2};
  array[3] int degree = {2, 2, 2};
}
parameters {
  real probe;
  array[3] real x;
  array[3] real probability;
}
model {
  if (probe > 0) {
    target += sum(exp(x[1:2]));
    target += sum(atan2(x[{3, 1}], probability[{3, 1}]));
    target += sum(atan2(x, probability));
    target += sum(tgamma(x));
    target += sum(bessel_first_kind(order, x));
    target += sum(bessel_second_kind(order, x));
    target += sum(binary_log_loss(label, probability));
    target += sum(falling_factorial(x, count));
    target += sum(ldexp(x, count));
    target += sum(lmgamma(degree, x));
    target += sum(modified_bessel_first_kind(order, x));
    target += sum(modified_bessel_second_kind(order, x));
    target += sum(rising_factorial(x, count));
    target += choose(degree[1], count[1]);
  }
}
