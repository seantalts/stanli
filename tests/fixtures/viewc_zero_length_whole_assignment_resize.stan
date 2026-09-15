parameters { real y; }
model {
  vector[0] a;
  a[:] = rep_vector(y, 1);
  target += y - a[1];
  y ~ normal(0, 1);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
