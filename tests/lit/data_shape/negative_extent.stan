// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: Found negative dimension size in variable declaration; variable=v; dimension size expression=N; expression value=-1
// STANLI-LIT-DATA: {"N": -1}
// CmdStan's message for a negative size, raised before later variables are reported missing.
data { int N; vector[N] v; simplex[2] s; real later; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
