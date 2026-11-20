// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: unsupported function gp_periodic_cov
parameters { real<lower=0> p; }
model { target += sum(gp_periodic_cov({1., 2.}, {1.5}, 1., 1., p)); }
