// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=M; position=0; dims declared=(2,3); dims found=(3,2)
// STANLI-LIT-DATA: {"M": [[1, 2], [3, 4], [5, 6]]}
data { matrix[2, 3] M; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
