// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// R=12: reroll's cost model needs Luse > 9 for a bare one-position term
// density to beat 40's margin at kLaneOpCost=5 per op (partition.cpp's own
// currencies); R=4 priced honestly and correctly declined.
// STANLI-LIT-DATA: {"R": 12, "C": 3, "x": [[0.1, 0.2, 0.3], [0.4, 0.5, 0.6], [0.7, 0.8, 0.9], [1.0, 1.1, 1.2], [1.3, 1.4, 1.5], [1.6, 1.7, 1.8], [1.9, 2.0, 2.1], [2.2, 2.3, 2.4], [2.5, 2.6, 2.7], [2.8, 2.9, 3.0], [3.1, 3.2, 3.3], [3.4, 3.5, 3.6]], "y": [[0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1]]}
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK-NOT: SET_SLICE_STRIDED
// STANLI-LIT-CHECK: BERNOULLI_LOGIT_LPMF
// STANLI-LIT-CHECK-NOT: BERNOULLI_LOGIT_LPMF
data {
  int R;
  int C;
  matrix[R, C] x;
  array[R, C] int y;
}
parameters {
  vector[3] b;
}
transformed parameters {
  matrix[R, C] p;
  for (j in 1 : R) {
    p[j, :] = fma(b[3], x[j, :], b[1]);
  }
}
model {
  b ~ normal(0, 10);
  for (i in 1 : R) {
    target += bernoulli_logit_lupmf(y[i] | p[i]);
  }
}
