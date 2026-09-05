// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 8, "y": [0.4, -1.1, 0.7, 2.0, -0.3, 1.4, 0.1, -0.9]}
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK-NOT: NORMAL_LPDF.v=0x86 s{{[0-9]+}}[1] s
// STANLI-LIT-CHECK: s{{[0-9]+}}[1] = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[8] s{{[0-9]+}}[1,P] s{{[0-9]+}}[1]
data {
  int<lower=0> N;
  array[N] real y;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  for (n in 1 : N) {
    y[n] ~ normal(mu, sigma);
  }
}
