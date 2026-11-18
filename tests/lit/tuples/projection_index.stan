// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 8, "y": [-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=42 
// STANLI-LIT-CHECK: = GATHER s{{[0-9]+}}[24] idata=[1,4,7,10,13,16,19]
// STANLI-LIT-CHECK: = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[7] s{{[0-9]+}}[7] s{{[0-9]+}}[7]
// projection_index_twin.stan is this model with the tuple split by hand.
// Both build the same graph, so keep their checks identical.
data {
  int N;
  array[N] real y;
}
parameters {
  vector[3] mu;
  real<lower=0> s;
}
model {
  array[N] tuple(real, vector[3]) z;
  for (n in 1:N) z[n] = (s * n, mu * n);
  for (n in 1:N) y[n] ~ normal(z[n].2[2], z[n].1 + s);
  mu ~ normal(0, 1);
}
