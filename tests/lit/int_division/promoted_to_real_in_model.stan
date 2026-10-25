// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"n": 3}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: fills:
// STANLI-LIT-CHECK-NOT: = DIV
// STANLI-LIT-CHECK: NORMAL_LPDF
data {
  int n;
}
parameters {
  real<lower=-1 / (n - 1), upper=1> rho;
}
model {
  rho ~ normal(7 / 2, 1);
}
