// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"R": 3, "C": 4, "x": [[0.1, 0.2, 0.3, 0.4], [0.5, 0.6, 0.7, 0.8], [0.9, 1.0, 1.1, 1.2]], "idx": [3, 1]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: SET_SLICE s{{[0-9]+}}[6]
// STANLI-LIT-CHECK-NOT: SET_INDEX s{{[0-9]+}}[6]
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
  vector[R] v = x[ : , 1] + mu;
  array[R] vector[2] z;
  array[R, 2] vector[2] a;
  for (i in 1 : R) {
    z[i] = b;
    for (j in 1 : 2) {
      a[i, j] = b;
    }
  }
  z[2, : ] = b + mu;
  target += sum(v) + sum(z[1]) + sum(a[1, 1]);
  mu ~ normal(0, 1);
  b ~ normal(0, 1);
}
