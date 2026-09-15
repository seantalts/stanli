// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 4, "y": [0.4, -1.1, 0.7, 2.0]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: NORMAL_LPDF.v={{0x[0-9a-f]+}} s{{[0-9]+}}[1] s
// STANLI-LIT-CHECK: s{{[0-9]+}}[1] = NORMAL_LPDF.v={{0x[0-9a-f]+}} s{{[0-9]+}}[4] s{{[0-9]+}}[1,P] s{{[0-9]+}}[1]
data {
  int<lower=0> N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) {
    y[n] ~ normal(mu, 1);
  }
}
