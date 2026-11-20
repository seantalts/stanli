// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  array[3] real theta;
}
model {
  array[0] real e = theta[4:];
  target += num_elements(theta[4:]);
  theta ~ normal(0, 1);
}
