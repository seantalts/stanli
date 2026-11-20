// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: FAIL dirichlet_rng: prior sample sizes has size 0, but must have a non-zero size
parameters { real k; }
model { k ~ normal(0, 1); }
generated quantities {
  vector[0] d = dirichlet_rng(rep_vector(1, 0));
  real z = normal_rng(k, 1);
}
