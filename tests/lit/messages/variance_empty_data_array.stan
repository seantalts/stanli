// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL variance: m has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0, "v": []}
data {
  int N;
  array[N] real v;
}
parameters {
  real t;
}
model {
  t ~ normal(0, 1);
  target += t * variance(v);
}
