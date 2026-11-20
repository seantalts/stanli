// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL max: int vector has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0, "k": []}
data {
  int N;
  array[N] int k;
}
parameters { real mu; }
model {
  mu ~ normal(0, 1);
  target += max(k) * 0.5;
}
