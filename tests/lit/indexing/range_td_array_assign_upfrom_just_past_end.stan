// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  array[3] real b = {1, 2, 3};
  array[0] real e;
  b[4:] = e;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
