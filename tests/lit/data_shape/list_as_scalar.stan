// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=v; dims declared=(1); dims found=()
// STANLI-LIT-DATA: {"v": 7}
data { vector[1] v; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
