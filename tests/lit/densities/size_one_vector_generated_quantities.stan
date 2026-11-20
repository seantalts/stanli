// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: FAIL normal_lpdf: Random variable has size = 1, but Location parameter has size 3; and they must be the same size.
// In generated quantities the mismatch rejects write_array, not the log
// density.
parameters { vector[3] mu; }
model { mu ~ normal(0, 1); }
generated quantities { vector[1] z = [1]'; real l = normal_lpdf(z | mu, 1); }
