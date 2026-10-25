// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  array[3] int idx = sort_indices_asc({1.0, not_a_number(), 2.0});
  array[3] int jdx = sort_indices_desc([1.0, not_a_number(), 2.0]');
}
parameters { real y; }
model { y ~ normal(0, 1); }
