// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=x; dims declared=(3); dims found=(1,3)
// STANLI-LIT-DATA: {"x": [[0.2, 0.3, 0.5]]}
data { simplex[3] x; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
