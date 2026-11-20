// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL matrix[min] row indexing: accessing element out of range. index 3 out of range; expecting index to be between 1 and 2
parameters {
  matrix[2, 3] theta;
}
model {
  target += sum(theta[3:]);
  to_vector(theta) ~ normal(0, 1);
}
