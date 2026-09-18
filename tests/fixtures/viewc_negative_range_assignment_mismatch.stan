parameters { real y; }
model {
  vector[2] a = rep_vector(y, 2);
  a[1:0] = rep_vector(y, 1);
  target += a[1];
  y ~ normal(0, 1);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL stanli compile: assignment width mismatch for a
