// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DUMP: log_prob:inplace
// STANLI-LIT-CHECK: GP_COV.v=0x04 s{{[0-9]+}}[3] s{{[0-9]+}}[1] s{{[0-9]+}}[1] s{{[0-9]+}}[1] s{{[0-9]+}}[1] idata=[3,1]
// STANLI-LIT-CHECK-NOT: SET_INDEX
parameters {
  real<lower=0> sigma;
  real<lower=0> l;
  real<lower=0> p;
  real<lower=0> tau;
}
model {
  matrix[3, 3] K = gp_periodic_cov({0.1, 0.9, 1.7}, sigma, l, p);
  for (i in 1:3) K[i, i] += tau;
  target += sum(K);
}
