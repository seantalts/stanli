// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "y": [0.4, -1.1, 0.7]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: SET_
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  vector[N] x = rep_vector(0, N);
  x[1 : 0] = log1p_exp(y[1 : 0]) - mu;
  y ~ normal(mu + x, 1);
}
