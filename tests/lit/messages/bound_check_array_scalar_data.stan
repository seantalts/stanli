// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL x[2][3] is -1, but must be greater than or equal to 0.000000
transformed data {
  array[2, 3] real<lower=0> x = {{1, 1, 1}, {1, 1, -1}};
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
