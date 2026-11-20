// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: FAIL lkj_corr_cholesky_rng: Dimension is 0, but must be positive!
// STANLI-LIT-TIMEOUT: 10
// Stan Math loops K - 1 times over an unsigned K, so K = 0 never ends.
parameters { real k; }
model { k ~ normal(0, 1); }
generated quantities {
  matrix[0, 0] d = lkj_corr_cholesky_rng(0, 2);
  real z = normal_rng(k, 1);
}
