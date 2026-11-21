// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL normal_lpdf: Random variable has size = 1, but Location parameter has size 3; and they must be the same size
// A vector of one element is a container, not a scalar: it must match the
// other containers' size. normal runs through a fused kernel.
parameters { vector[1] y; vector[3] mu; }
model { y ~ normal(mu, 1); mu ~ normal(0, 1); }
