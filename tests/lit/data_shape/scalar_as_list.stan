// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=s; dims declared=(); dims found=(1)
// STANLI-LIT-DATA: {"s": [7]}
data { real s; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
