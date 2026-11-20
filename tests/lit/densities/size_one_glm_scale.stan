// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL normal_id_glm_lpdf: Vector of scale parameters has dimension = 1, expecting dimension = 3
// STANLI-LIT-DATA: {"N": 3, "y": [1, 2, 3], "X": [[1, 2], [3, 4], [5, 6]]}
// normal_id_glm's scale is a scalar or one value per row.
data { int N; vector[N] y; matrix[N, 2] X; }
parameters { real a; vector[2] b; vector<lower=0>[1] s; }
model { y ~ normal_id_glm(X, a, b, s); }
