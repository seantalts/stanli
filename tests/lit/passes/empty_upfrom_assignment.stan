// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "K": 0, "y": [0.4, -1.1, 0.7], "idx": []}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: SET_
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
  x[N + 1 : ] = log1p_exp(y[N + 1 : ]) - mu;
  y ~ normal(mu + x, 1);
}
