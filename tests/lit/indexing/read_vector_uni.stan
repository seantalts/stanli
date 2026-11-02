// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[uni] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"k": 4}
data {
  int k;
}
transformed data {
  vector[3] v = [1, 2, 3]';
  real x = v[k];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
