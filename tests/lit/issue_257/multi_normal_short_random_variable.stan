// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL stanli compile: multi_normal_lpdf: vectorized argument must be a matching vector or array of vectors
// STANLI-LIT-DATA: {"mu": [0, 0, 0], "S": [[1, 0, 0], [0, 1, 0], [0, 0, 1]]}
data { vector[3] mu; cov_matrix[3] S; }
parameters { vector[2] y; }
model { y ~ multi_normal(mu, S); }
