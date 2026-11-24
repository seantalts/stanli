// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: s is not a valid simplex
// STANLI-LIT-DATA: {"N": 2, "v": [1, 2], "s": [0.9, 0.9], "later": [1, 2]}
// CmdStan checks each variable's constraints before the shape of the next one.
data { int N; vector[N] v; simplex[2] s; real later; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
