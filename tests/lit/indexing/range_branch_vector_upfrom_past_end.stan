// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL vector[min] indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
parameters {
  real theta;
}
model {
  vector[3] v = [1, 2, 3]';
  if (theta > -100) target += sum(v[4:]);
  theta ~ normal(0, 1);
}
