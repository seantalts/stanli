// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"y": [1, 0, 4], "z": [2, 3, 1], "phi": [0.2, 0.3, 0.5]}
// STANLI-LIT-DUMP: log_prob:lower
// The multinomial coefficient of the counts is computed once, and the
// density reads it as a second input when the probabilities are parameters.
// STANLI-LIT-CHECK: = MULTINOMIAL_LPMF.v=0x01 s{{[0-9]+}}[3] s{{[0-9]+}}[1] idata=[1,0,4]
// STANLI-LIT-CHECK: = MULTINOMIAL_LOGIT_LPMF.v=0x01 s{{[0-9]+}}[3,P] s{{[0-9]+}}[1] idata=[2,3,1]
// STANLI-LIT-CHECK: = MULTINOMIAL_LOGIT_LPMF.v=0x81 s{{[0-9]+}}[3,P] idata=[1,0,4]
// STANLI-LIT-CHECK: = MULTINOMIAL_LPMF s{{[0-9]+}}[3] idata=[2,3,1]
data {
  array[3] int y;
  array[3] int z;
  simplex[3] phi;
}
parameters {
  simplex[3] theta;
  vector[3] mu;
}
model {
  mu ~ normal(0, 1);
  target += multinomial_lpmf(y | theta);
  target += multinomial_logit_lpmf(z | mu);
  y ~ multinomial_logit(mu);
  target += multinomial_lpmf(z | phi);
}
