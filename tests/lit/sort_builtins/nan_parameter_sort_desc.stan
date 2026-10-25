// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: sort_asc
parameters { real y; }
model {
  array[3] real v = {1, y * not_a_number(), 2};
  array[3] real s = sort_desc(v);
  y ~ normal(0, 1);
  target += s[1];
}
