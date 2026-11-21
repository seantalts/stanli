// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL vector[min] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
parameters {
  vector[3] theta;
}
model {
  target += sum(theta[4:]);
  theta ~ normal(0, 1);
}
