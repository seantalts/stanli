// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 2, "K": 3, "Y": [[1, 0, 4], [2, 3, 0]]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK-NOT: = SOFTMAX
// STANLI-LIT-CHECK: = MULTINOMIAL_SOFTMAX_LPMF.v=0x01 s{{[0-9]+}}[3] s{{[0-9]+}}[1] idata=[1,0,4]
// STANLI-LIT-CHECK-NOT: = SOFTMAX
// STANLI-LIT-CHECK: = MULTINOMIAL_SOFTMAX_LPMF.v=0x01 s{{[0-9]+}}[3] s{{[0-9]+}}[1] idata=[2,3,0]
// STANLI-LIT-CHECK-NOT: = SOFTMAX
data {
  int<lower=1> N;
  int<lower=1> K;
  array[N, K] int Y;
}
parameters {
  array[N] vector[K] mu;
}
model {
  for (n in 1:N) {
    mu[n] ~ normal(0, 1);
    target += multinomial_lpmf(Y[n] | softmax(mu[n]));
  }
}
