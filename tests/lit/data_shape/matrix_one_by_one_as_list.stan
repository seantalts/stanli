// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=m; dims declared=(1,1); dims found=(1)
// STANLI-LIT-DATA: {"m": [1.0]}
data { matrix[1, 1] m; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
