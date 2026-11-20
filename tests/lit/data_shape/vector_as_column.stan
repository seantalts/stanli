// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=v; dims declared=(3); dims found=(3,1)
// STANLI-LIT-DATA: {"v": [[1], [2], [3]]}
data { vector[3] v; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
