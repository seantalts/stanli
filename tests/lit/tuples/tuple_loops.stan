// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 4, "obs": [{"1": 0.5, "2": 1}, {"1": -0.25, "2": 0}, {"1": 1.5, "2": 2}, {"1": 0.0, "2": 1}], "y": [0.1, -0.4, 1.2, 0.3]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=29 
// STANLI-LIT-CHECK: = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[4] s{{[0-9]+}}[4] s{{[0-9]+}}[4]
// STANLI-LIT-CHECK-NOT: NORMAL_LPDF.v=0x86 s{{[0-9]+}}[1]
// tuple_loops_twin.stan is this model with the tuples split by hand. Both
// build the same graph, so keep their checks identical.
data {
  int N;
  array[N] tuple(real, int) obs;
  vector[N] y;
}
parameters {
  tuple(real, real<lower=0>) coef;
  real<lower=0> sigma;
}
model {
  array[N] tuple(real, real) fitted;
  for (n in 1:N) {
    fitted[n].1 = coef.1 + coef.2 * obs[n].1;
    fitted[n].2 = sigma * (1 + obs[n].2);
  }
  for (n in 1:N) y[n] ~ normal(fitted[n].1, fitted[n].2);
  coef.1 ~ normal(0, 2);
  coef.2 ~ lognormal(0, 1);
  sigma ~ exponential(1);
}
