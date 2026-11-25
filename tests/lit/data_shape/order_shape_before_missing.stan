// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=v; position=0; dims declared=(2); dims found=(3)
// STANLI-LIT-DATA: {"N": 2, "v": [1, 2, 3]}
// CmdStan checks each variable's shape before it reports a later one as missing.
data { int N; vector[N] v; simplex[2] s; real later; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
