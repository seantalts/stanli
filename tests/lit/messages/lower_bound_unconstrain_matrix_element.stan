// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL lb_free: Lower bounded variable[2, 1] is 3, but must be greater than or equal to 3.500000
transformed data {
  matrix[2, 2] m = [[4, 5], [3, 6]];
  matrix[2, 2] u = lower_bound_unconstrain(m, 3.5);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
