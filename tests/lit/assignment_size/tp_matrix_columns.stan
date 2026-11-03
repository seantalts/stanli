// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL matrix assign columns: assigning variable tp_m23 (3) and right hand side columns (4) must match in size
parameters {
  real y;
}
transformed parameters {
  matrix[2, 3] tp_m23 = [[y, 2, 3, 4], [4, 5, 6, 7]];
}
model {
  y ~ normal(0, 1);
}
