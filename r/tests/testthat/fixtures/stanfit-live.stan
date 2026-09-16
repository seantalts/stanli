
parameters {
  real a;
  real<lower=0> b;
  real<lower=-2,upper=3> z;
  ordered[3] o;
  simplex[3] p;
  corr_matrix[2] C;
  matrix[2,2] M;
  array[2] vector[2] av;
  array[2] matrix[2,3] am;
}
transformed parameters { real derived = a + b; }
model {
  a ~ std_normal(); b ~ lognormal(0,1);
  o ~ std_normal(); p ~ dirichlet([1,2,3]');
  C ~ lkj_corr(2); to_vector(M) ~ std_normal();
  for (i in 1:2) { av[i] ~ std_normal(); to_vector(am[i]) ~ std_normal(); }
}
generated quantities { vector[2] log_lik; log_lik[1] = normal_lpdf(-1 | a, b); log_lik[2] = normal_lpdf(1 | a, b); }
