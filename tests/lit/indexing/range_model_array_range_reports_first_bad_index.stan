// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL array[..., ...] index: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
parameters {
  array[3] real theta;
}
model {
  target += sum(theta[2:6]);
  theta ~ normal(0, 1);
}
