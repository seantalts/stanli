// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: sort_asc
parameters { real y; }
model {
  vector[3] v = [1, y * not_a_number(), 2]';
  vector[3] s = sort_asc(v);
  y ~ normal(0, 1);
  target += s[1];
}
