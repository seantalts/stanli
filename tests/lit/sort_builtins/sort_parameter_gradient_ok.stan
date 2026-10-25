// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters { vector[3] p; }
model {
  p ~ normal(0, 1);
  target += dot_product(sort_desc(p), [3, 2, 1]');
}
