// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: sort_asc
transformed data {
  array[3] real v = {1, not_a_number(), 2};
  array[3] real s = sort_desc(v);
}
parameters { real y; }
model { y ~ normal(0, 1); }
