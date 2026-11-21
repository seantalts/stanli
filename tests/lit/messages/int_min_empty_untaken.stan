// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 0, "k": []}
data {
  int N;
  array[N] int k;
}
parameters { real mu; }
model {
  mu ~ normal(0, 1);
  if (N > 0) target += min(k);
}
