// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL multi_normal_cholesky_lpdf: Size of random variable (3) and size of location parameter (2) must match in size
// STANLI-LIT-DATA: {"mu": [0, 0], "L": [[1, 0, 0], [0, 1, 0], [0, 0, 1]]}
data { vector[2] mu; cholesky_factor_cov[3] L; }
parameters { vector[3] y; }
model { y ~ multi_normal_cholesky(mu, L); }
