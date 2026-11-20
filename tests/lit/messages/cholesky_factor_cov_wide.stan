// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL cholesky_factor_cov L: num rows (must be greater or equal to num cols) is 2, but must be greater than or equal to 3
// STANLI-LIT-DATA: {"M": 2, "N": 3}
data { int M; int N; }
parameters {
  cholesky_factor_cov[M, N] L;
  real mu;
}
model { mu ~ normal(0, 1); }
