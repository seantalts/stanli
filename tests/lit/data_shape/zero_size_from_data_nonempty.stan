// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in dimension declared and found in context; processing stage=data initialization; variable name=y; position=0; dims declared=(0); dims found=(1)
// STANLI-LIT-DATA: {"N": 0, "y": [1.0]}
data {
  int N;
  vector[N] y;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
