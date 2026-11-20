// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL array[..., ...] indexing: size is -1, but must be greater than or equal to 0
parameters {
  real theta;
}
model {
  array[3] real a = {1, 2, 3};
  if (theta > -100) target += sum(a[5:]);
  theta ~ normal(0, 1);
}
