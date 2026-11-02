// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL ub_free: Upper bounded variable[3] is 9, but must be less than or equal to 5.000000
transformed data {
  vector[3] v = [1, 2, 9]';
  vector[3] u = upper_bound_unconstrain(v, 5);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
