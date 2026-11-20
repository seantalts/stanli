// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "y": [0.4, -1.1, 0.7]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: SET_
// On an array, `N + 1:` selects nothing. A vector rejects it, as Stan does.
data {
  int N;
  array[N] real y;
}
parameters {
  real mu;
}
model {
  array[N] real x = y;
  x[N + 1 : ] = y[N + 1 : ];
  y ~ normal(mu + to_vector(x), 1);
}
