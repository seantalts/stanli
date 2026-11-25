// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 12, "J": 3, "K": 2, "subject": [2, 1, 3, 3, 1, 2, 2, 3, 1, 1, 3, 2], "X": [[1, 0.5], [1, -0.2], [1, 1.3], [1, 0.1], [1, -0.7], [1, 0.9], [1, 0.4], [1, -1.1], [1, 0.6], [1, 0.3], [1, -0.4], [1, 1.0]], "y": [0.2, -0.5, 1.1, 0.3, -0.8, 0.6, 0.1, -1.2, 0.4, 0.0, -0.3, 0.9]}
// STANLI-LIT-DUMP: log_prob:reroll
// The first lane writes mu functionally and stays scalar; the other 11 fold.
// STANLI-LIT-CHECK: s{{[0-9]+}}[22] = GATHER s{{[0-9]+}}[6,P] idata=
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[22] = REP_MAT s{{[0-9]+}}[2,P] idata=[2,11,1]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[22] = ADD
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[11] = GROUP_DOT.v=0x01 s{{[0-9]+}}[22] s{{[0-9]+}}[22] idata=[11,2,2,1]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[12] = SET_SLICE
// STANLI-LIT-CHECK-NOT: = DOT
data {
  int<lower=1> N;
  int<lower=1> J;
  int<lower=1> K;
  array[N] int<lower=1, upper=J> subject;
  matrix[N, K] X;
  vector[N] y;
}
parameters {
  matrix[K, J] z;
  vector[K] beta;
  real<lower=0> sigma;
}
transformed parameters {
  vector[N] mu;
  for (i in 1 : N) {
    mu[i] = X[i] * (beta + z[ : , subject[i]]);
  }
}
model {
  to_vector(z) ~ std_normal();
  beta ~ std_normal();
  sigma ~ cauchy(0, 1);
  y ~ student_t(4, mu, sigma);
}
