// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: sort_asc
transformed data {
  vector[3] v = [1, not_a_number(), 2]';
  vector[3] s = sort_asc(v);
}
parameters { real y; }
model { y ~ normal(0, 1); }
