// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: matrix[uni] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 2
// STANLI-LIT-DATA: {"k": 4, "m": [[1, 2], [3, 4]]}
data {
  int k;
  matrix[2, 2] m;
}
parameters {
  row_vector[2] y;
}
model {
  y ~ normal(m[k], 1);
}
