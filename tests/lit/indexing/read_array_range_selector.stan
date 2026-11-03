// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[..., ...] index: accessing element out of range. index 6 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"K": 6}
data {
  int K;
}
transformed data {
  array[3] real v = {1,2,3};array[2] real x = v[2:K];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
