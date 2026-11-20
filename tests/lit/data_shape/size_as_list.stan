// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: mismatch in number dimensions declared and found in context; processing stage=data initialization; variable name=N; dims declared=(); dims found=(1)
// STANLI-LIT-DATA: {"N": [3], "y": [1, 2, 3]}
data {
  int N;
  vector[N] y;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
