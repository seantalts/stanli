// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=atp.1; dims declared=(2,2); dims found=(2,1,2)
// STANLI-LIT-DATA: {"atp": [{"1": [[1.0, 2.0]], "2": 3}, {"1": [[4.0, 5.0]], "2": 6}]}
data { array[2] tuple(vector[2], int) atp; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
