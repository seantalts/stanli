// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 4, "obs_1": [0.5, -0.25, 1.5, 0.0], "obs_2": [1, 0, 2, 1], "y": [0.1, -0.4, 1.2, 0.3]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=29 
// STANLI-LIT-CHECK: = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[4] s{{[0-9]+}}[4] s{{[0-9]+}}[4]
// STANLI-LIT-CHECK-NOT: NORMAL_LPDF.v=0x86 s{{[0-9]+}}[1]
// tuple_loops.stan is this model with tuples. Both build the same graph, so
// keep their checks identical.
data {
  int N;
  array[N] real obs_1;
  array[N] int obs_2;
  vector[N] y;
}
parameters {
  real coef_1;
  real<lower=0> coef_2;
  real<lower=0> sigma;
}
model {
  array[N] real fitted_1;
  array[N] real fitted_2;
  for (n in 1:N) {
    fitted_1[n] = coef_1 + coef_2 * obs_1[n];
    fitted_2[n] = sigma * (1 + obs_2[n]);
  }
  for (n in 1:N) y[n] ~ normal(fitted_1[n], fitted_2[n]);
  coef_1 ~ normal(0, 2);
  coef_2 ~ lognormal(0, 1);
  sigma ~ exponential(1);
}
