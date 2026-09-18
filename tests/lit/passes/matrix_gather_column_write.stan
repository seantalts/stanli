// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"R": 3, "C": 4, "x": [[0.1, 0.2, 0.3, 0.4], [0.5, 0.6, 0.7, 0.8], [0.9, 1.0, 1.1, 1.2]], "idx": [3, 1]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: SET_INDEX
// STANLI-LIT-CHECK-NOT: SET_SLICE
data {
  int R;
  int C;
  matrix[R, C] x;
  array[2] int idx;
}
parameters {
  real mu;
  vector[2] b;
}
model {
  matrix[R, C] m = x + mu;
  m[idx, 2] = b;
  target += sum(m);
  mu ~ normal(0, 1);
  b ~ normal(0, 1);
}
