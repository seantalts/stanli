// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[min] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
transformed data {
  vector[3] v = [1, 2, 3]';
  vector[0] w = v[4:];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
