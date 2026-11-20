// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[multi, ...] assign: assigning variable b (-1) and right hand side size (0) must match in size
transformed data {
  array[3] real b = {1, 2, 3};
  array[0] real e;
  b[5:] = e;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
