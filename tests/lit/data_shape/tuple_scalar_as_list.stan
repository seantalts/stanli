// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=tp.1; dims declared=(); dims found=(1)
// STANLI-LIT-DATA: {"tp": {"1": [1.5], "2": [1, 2]}}
data { tuple(real, array[2] int) tp; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
