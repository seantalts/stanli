// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL normal_lcdf: Random variable has size = 3, but Location parameter has size 1; and they must be the same size
// STANLI-LIT-DATA: {"y": [1, 2, 3]}
// The same size rule for a distribution function.
data { vector[3] y; }
parameters { vector[1] mu; }
model { target += normal_lcdf(y | mu, 1); mu ~ normal(0, 1); }
