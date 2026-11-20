// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"S": [[2, 0, 0], [0, 2, 0], [0, 0, 2]], "C": [[1, 0], [0, 1]], "L": [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1]], "L2": [[1, 0], [0, 1]], "x": [0.2, 0.3, 0.5], "tp": {"1": 1.5, "2": [1, 2]}, "atp": [{"1": [1.0, 2.0], "2": 3}, {"1": [4.0, 5.0], "2": 6}]}
// Constrained matrices take their implied extents; tuple members are checked
// one by one.
data {
  cov_matrix[3] S;
  corr_matrix[2] C;
  cholesky_factor_cov[4, 3] L;
  cholesky_factor_cov[2] L2;
  simplex[3] x;
  tuple(real, array[2] int) tp;
  array[2] tuple(vector[2], int) atp;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
