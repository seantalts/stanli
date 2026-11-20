// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=a; dims declared=(2,3); dims found=(6)
// STANLI-LIT-DATA: {"a": [1, 2, 3, 4, 5, 6]}
data { array[2, 3] real a; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
