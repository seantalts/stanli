// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "K": 0, "y": [0.4, -1.1, 0.7], "idx": []}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: INDEX
data {
  int N;
  int K;
  vector[N] y;
  array[K] int idx;
}
parameters {
  real mu;
}
model {
  vector[N] x = y + mu;
  target += sum(log1p_exp(x[1 : 0])) + sum(log1p_exp(x[idx]));
  y ~ normal(mu + x, 1);
}
