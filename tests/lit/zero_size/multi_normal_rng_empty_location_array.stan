// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: FAIL multi_normal_rng: Size of location parameter (0) and rows of covariance parameter (2) must match in size
// The interpreter passes the flattened array as one location. Stan Math
// does not compare its size with the covariance, so the draw used to write
// past the end of a buffer.
parameters { real k; }
model { k ~ normal(0, 1); }
generated quantities {
  array[0] vector[2] mu;
  array[0] vector[2] d = multi_normal_rng(mu, diag_matrix(rep_vector(1, 2)));
  real z = normal_rng(k, 1);
}
