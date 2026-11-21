// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL poisson_log_glm_lpmf: Vector of intercepts has dimension = 1, expecting dimension = 3
// STANLI-LIT-DATA: {"N": 3, "y": [1, 2, 3], "X": [[1, 2], [3, 4], [5, 6]]}
// A GLM's intercept is a scalar or one value per row of the design matrix.
data { int N; array[N] int y; matrix[N, 2] X; }
parameters { vector[1] a; vector[2] b; }
model { y ~ poisson_log_glm(X, a, b); }
