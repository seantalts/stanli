// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL segment: n is 5, but must be less than or equal to 3
// STANLI-LIT-DATA: {"v": [1, 2, 3]}
data {
  vector[3] v;
}
parameters {
  real p;
}
model {
  segment(v, 2, 4) ~ normal(0, 1);
}
