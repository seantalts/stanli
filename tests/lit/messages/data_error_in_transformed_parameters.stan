// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL segment: i is 0, but must be greater than 0
// STANLI-LIT-DATA: {"ts": [1, 2, 3]}
data {
  array[3] real ts;
}
parameters {
  real p;
}
transformed parameters {
  array[0] real s = segment(ts, 0, 0);
}
model {
  p ~ normal(0, 1);
}
