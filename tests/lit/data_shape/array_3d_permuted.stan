// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=a; position=0; dims declared=(2,3,1); dims found=(3,2,1)
// STANLI-LIT-DATA: {"a": [[[1], [2]], [[3], [4]], [[5], [6]]]}
// Removing the extent of 1 leaves (2, 3) against (3, 2), which still differ.
data { array[2, 3, 1] real a; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
