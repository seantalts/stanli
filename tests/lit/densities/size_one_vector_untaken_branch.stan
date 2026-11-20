// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Stan rejects the size mismatch only when the call runs.
parameters { vector[1] y; vector[3] mu; }
model { mu ~ normal(0, 1); y ~ normal(0, 1); if (y[1] > 10) target += normal_lpdf(y | mu, 1); }
