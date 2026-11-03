// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[multi] indexing: accessing element out of range. index 7 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"K": 7}
data {
  int K;
}
transformed data {
  vector[3] v = [1,2,3]';array[2] int ix = {1, 7};vector[2] x = v[ix];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
