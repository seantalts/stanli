// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL vector[min] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
parameters {
  array[2] vector[3] theta;
}
model {
  target += sum(theta[1, 4:]);
  theta[1] ~ normal(0, 1);
  theta[2] ~ normal(0, 1);
}
