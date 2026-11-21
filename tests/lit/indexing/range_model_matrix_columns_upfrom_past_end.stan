// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL matrix[..., min] column indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3
parameters {
  matrix[2, 3] theta;
}
model {
  target += sum(theta[:, 4:]);
  to_vector(theta) ~ normal(0, 1);
}
