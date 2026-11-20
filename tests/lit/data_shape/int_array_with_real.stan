// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: int variable contained non-int values; processing stage=data initialization; variable name=ai; base type=int
// STANLI-LIT-DATA: {"ai": [1.0, 2]}
data { array[2] int ai; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
