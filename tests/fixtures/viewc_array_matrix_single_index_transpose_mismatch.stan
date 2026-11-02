parameters { real y; }
model {
  array[1] matrix[2, 3] a;
  a[1] = rep_matrix(y, 3, 2);
  target += a[1, 1, 1];
  y ~ normal(0, 1);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL matrix assign columns: assigning variable a (3) and right hand side columns (2) must match in size
