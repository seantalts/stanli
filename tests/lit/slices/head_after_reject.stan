// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL first
// STANLI-LIT-DATA: {"v": [1, 2, 3]}
data {
  row_vector[3] v;
}
parameters {
  real p;
}
model {
  reject("first");
  head(v, 5) ~ normal(0, 1);
}
