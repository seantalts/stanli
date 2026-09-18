// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"R": 3, "C": 4, "x": [[0.1, 0.2, 0.3, 0.4], [0.5, 0.6, 0.7, 0.8], [0.9, 1.0, 1.1, 1.2]], "y": [[0, 1, 0, 1], [1, 1, 0, 0], [0, 0, 1, 1]]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: SET_SLICE_STRIDED
// STANLI-LIT-CHECK-NOT: SET_INDEX
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
    for (t in 1 : C) {
      y[i, t] ~ bernoulli_logit(p[i, t]);
    }
  }
}
