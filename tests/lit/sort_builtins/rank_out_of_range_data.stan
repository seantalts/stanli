// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: out of range
transformed data {
  int r = rank({1.0, 2.0, 3.0}, 4);
}
parameters { real y; }
model { y ~ normal(0, 1); }
