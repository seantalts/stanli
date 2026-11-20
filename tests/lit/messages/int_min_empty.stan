// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL min: int vector has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0, "k": []}
// Stan Math rejects the empty int array when the call runs, not when the
// model loads.
data {
  int N;
  array[N] int k;
}
parameters { real mu; }
model { mu ~ normal(min(k), 1); }
