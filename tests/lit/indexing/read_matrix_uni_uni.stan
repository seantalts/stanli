// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL matrix[uni,uni] column indexing: accessing element out of range. index 3 out of range; expecting index to be between 1 and 2
// STANLI-LIT-DATA: {"k": 3}
data {
  int k;
}
transformed data {
  matrix[2, 2] m = [[1, 2], [3, 4]];
  real x = m[1, k];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
