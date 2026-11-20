// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL mean: m has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0}
data {
  int N;
}
parameters {
  real t;
  vector[N] v;
}
model {
  t ~ normal(0, 1);
  target += 0 * mean(v);
}
