// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL first
// STANLI-LIT-DATA: {"v": [1, 2, 3]}
data {
  vector[3] v;
}
parameters {
  real p;
}
model {
  reject("first");
  segment(v, 2, 4) ~ normal(0, 1);
}
