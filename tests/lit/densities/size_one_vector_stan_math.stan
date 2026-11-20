// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL gumbel_lpdf: Random variable has size = 3, but Location parameter has size 1; and they must be the same size
// The same size rule for a density that calls Stan Math's own function.
parameters { vector[3] y; vector[1] mu; }
model { y ~ gumbel(mu, 1); mu ~ normal(0, 1); }
