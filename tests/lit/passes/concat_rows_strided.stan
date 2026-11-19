// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Each row is `[0, mu2[n], mu3[n]]`. Re-roll writes the rows' columns into
// one row-major matrix with strided stores, rather than concatenating
// column-major columns and transposing the result.
// STANLI-LIT-DATA: {"N": 8, "Y": [1, 3, 2, 2, 3, 1, 1, 3], "x": [0.5, -1.0, 0.25, 2.0, -0.75, 1.5, 0.0, -2.0]}
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK-NOT: CONCAT2
// STANLI-LIT-CHECK-NOT: TRANSPOSE
// STANLI-LIT-CHECK: s{{[0-9]+}}[24] = REP_MAT s{{[0-9]+}}[1] idata=[3,8,0]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[24] = SET_SLICE_STRIDED_INPLACE s{{[0-9]+}}[24] s{{[0-9]+}}[8] idata=[1,3]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[24] = SET_SLICE_STRIDED_INPLACE s{{[0-9]+}}[24] s{{[0-9]+}}[8] idata=[2,3]
// STANLI-LIT-CHECK-NOT: CONCAT2
// STANLI-LIT-CHECK-NOT: TRANSPOSE
data {
  int N;
  array[N] int Y;
  vector[N] x;
}
parameters {
  real a2;
  real a3;
  real b2;
  real b3;
}
model {
  vector[N] mu2 = a2 + b2 * x;
  vector[N] mu3 = a3 + b3 * x;
  array[N] vector[3] mu;
  for (n in 1:N) {
    mu[n] = transpose([0, mu2[n], mu3[n]]);
  }
  for (n in 1:N) {
    target += categorical_logit_lpmf(Y[n] | mu[n]);
  }
  target += normal_lpdf(a2 | 0, 1) + normal_lpdf(a3 | 0, 1);
  target += normal_lpdf(b2 | 0, 1) + normal_lpdf(b3 | 0, 1);
}
