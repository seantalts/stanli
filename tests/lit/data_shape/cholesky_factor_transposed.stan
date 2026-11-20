// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=L; position=0; dims declared=(4,3); dims found=(3,4)
// STANLI-LIT-DATA: {"L": [[1, 0, 0, 1], [0, 1, 0, 1], [0, 0, 1, 1]]}
data { cholesky_factor_cov[4, 3] L; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
