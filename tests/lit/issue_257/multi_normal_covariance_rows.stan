// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL multi_normal_lpdf: Size of random variable (3) and rows of covariance parameter (2) must match in size
// STANLI-LIT-DATA: {"mu": [0, 0, 0], "S": [[1, 0], [0, 1]]}
data { vector[3] mu; cov_matrix[2] S; }
parameters { vector[3] y; }
model { y ~ multi_normal(mu, S); }
