// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[uni, ...] index: accessing element out of range. index 0 out of range; expecting index to be between 1 and 4
// STANLI-LIT-DATA: {"k": 0}
data {
  int k;
}
transformed data {
  array[4] int zs = {1, 2, 3, 4};
  int x = zs[k];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
