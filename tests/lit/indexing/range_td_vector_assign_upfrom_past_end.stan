// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[min] assign: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
transformed data {
  vector[3] w = [1, 2, 3]';
  vector[0] e;
  w[4:] = e;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
