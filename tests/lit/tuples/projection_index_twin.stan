// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 8, "y": [-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=26 
// STANLI-LIT-CHECK: = GATHER s{{[0-9]+}}[24] idata=[1,4,7,10,13,16,19,22]
// STANLI-LIT-CHECK: = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[8] s{{[0-9]+}}[8] s{{[0-9]+}}[8]
// projection_index.stan is this model with a tuple. Both build the same
// graph, so keep their checks identical.
data {
  int N;
  array[N] real y;
}
parameters {
  vector[3] mu;
  real<lower=0> s;
}
model {
  array[N] real z_1;
  array[N] vector[3] z_2;
  for (n in 1:N) {
    z_1[n] = s * n;
    z_2[n] = mu * n;
  }
  for (n in 1:N) y[n] ~ normal(z_2[n, 2], z_1[n] + s);
  mu ~ normal(0, 1);
}
