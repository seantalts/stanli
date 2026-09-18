parameters { real y; }
model {
  array[1] matrix[2, 3] a;
  matrix[3, 2] b = rep_matrix(y, 3, 2);
  a[1, :, :] = b;
  target += a[1, 1, 1];
  y ~ normal(0, 1);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL stanli compile: assignment logical view mismatch for a
