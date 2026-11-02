// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL vector assign rows: assigning variable v (2) and right hand side rows (3) must match in size
// STANLI-LIT-DATA: {"x": [1, 2, 3]}
data {
  vector[3] x;
}
parameters {
  real y;
}
model {
  vector[2] v = y * x;
  target += sum(v);
  y ~ normal(0, 1);
}
