// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Containers of one element each match one another.
parameters { vector[1] y; vector[1] mu; vector<lower=0>[1] s; }
model { y ~ normal(mu, s); target += gumbel_lpdf(y | mu, s); mu ~ normal(0, 1); s ~ lognormal(0, 1); }
