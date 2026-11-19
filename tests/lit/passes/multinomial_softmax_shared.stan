// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"y": [1, 0, 4]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: = SOFTMAX
// STANLI-LIT-CHECK: = MULTINOMIAL_LPMF.v=0x01 s{{[0-9]+}}[3] s{{[0-9]+}}[1] idata=[1,0,4]
// STANLI-LIT-CHECK-NOT: MULTINOMIAL_SOFTMAX_LPMF
data {
  array[3] int y;
}
parameters {
  vector[3] mu;
}
model {
  vector[3] theta = softmax(mu);
  mu ~ normal(0, 1);
  target += multinomial_lpmf(y | theta);
  target += -sum(theta);
}
