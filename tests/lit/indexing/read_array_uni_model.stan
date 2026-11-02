// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: array[uni, ...] index: accessing element out of range. index 5 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"k": 5, "x": [1, 2, 3]}
data {
  int k;
  array[3] real x;
}
parameters {
  real y;
}
model {
  y ~ normal(x[k], 1);
}
