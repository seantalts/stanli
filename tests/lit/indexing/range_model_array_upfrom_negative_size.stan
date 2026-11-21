// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL array[..., ...] indexing: size is -1, but must be greater than or equal to 0
parameters {
  array[3] real theta;
}
model {
  target += sum(theta[5:]);
  theta ~ normal(0, 1);
}
