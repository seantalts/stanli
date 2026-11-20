// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: FAIL hmm_latent_rng: log_omegas has size 0, but must have a non-zero size
parameters { real k; }
model { k ~ normal(0, 1); }
generated quantities {
  matrix[2, 0] log_omegas;
  matrix[2, 2] Gamma = [[0.9, 0.1], [0.2, 0.8]];
  vector[2] rho = [0.5, 0.5]';
  array[0] int d = hmm_latent_rng(log_omegas, Gamma, rho);
  real z = normal_rng(k, 1);
}
