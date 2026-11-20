// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector[min] assign: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"N": 3, "y": [0.4, -1.1, 0.7]}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  vector[N] x = y + mu;
  x[N + 1:] = log1p_exp(y[N + 1:N]) - mu;
  y ~ normal(mu + x, 1);
}
