// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: Variable: a, error: non-rectangular array
// STANLI-LIT-DATA: {"a": [[1, 2, 3], [4, 5]]}
data { array[2] vector[3] a; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
