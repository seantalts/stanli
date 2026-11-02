// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[uni, ...] index: accessing element out of range. index 3 out of range; expecting index to be between 1 and 2
// STANLI-LIT-DATA: {"k": 3}
data {
  int k;
}
transformed data {
  array[2, 2] real zs = {{1, 2}, {3, 4}};
  real x = zs[1, k];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
