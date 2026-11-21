// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[..., ...] indexing: size is -1, but must be greater than or equal to 0
transformed data {
  array[3] real a = {1, 2, 3};
  print(a[5:]);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
