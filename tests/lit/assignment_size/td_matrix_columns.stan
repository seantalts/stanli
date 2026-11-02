// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: matrix assign columns: assigning variable m (3) and right hand side columns (4) must match in size
transformed data {
  matrix[2, 3] m = [[1, 2, 3, 4], [4, 5, 6, 8]];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
