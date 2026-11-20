// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL mean: m has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0, "d": []}
// A data operand is evaluated while the model is prepared. Stan rejects the
// empty mean when log_prob runs, and so must stanli.
data {
  int N;
  vector[N] d;
}
parameters {
  real t;
}
model {
  t ~ normal(0, 1);
  target += mean(d);
}
