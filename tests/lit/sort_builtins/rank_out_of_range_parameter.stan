// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: out of range
parameters { vector[3] p; }
model {
  int r = rank(p, 0);
  p ~ normal(0, 1);
  target += r;
}
