// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data { print(gp_periodic_cov({1, 2}, 1., 1., 1.)); }
parameters { real mu; }
model { mu ~ normal(0, 1); }
