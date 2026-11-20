// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL normal_lpdf: Random variable has size = 1, but Location parameter has size 3; and they must be the same size
// The same mismatch in a branch that runs, in a runtime-control region.
parameters { vector[1] y; vector[3] mu; }
model { mu ~ normal(0, 1); y ~ normal(0, 1); if (y[1] > -10) target += normal_lpdf(y | mu, 1); }
