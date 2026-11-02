// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[min_max] max indexing: accessing element out of range. index 6 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"K": 6}
data {
  int K;
}
transformed data {
  vector[3] v = [1,2,3]';vector[2] x = v[2:K];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
