// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=Z; position=0; dims declared=(0,3); dims found=(1,3)
// STANLI-LIT-DATA: {"Z": [[1, 2, 3]]}
data { matrix[0, 3] Z; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
