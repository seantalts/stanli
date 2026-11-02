// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL csr_extract_w: the number of nonzero entries depends on the values, so the matrix must be data
parameters {
  matrix[2, 2] m;
}
model {
  vector[4] w = csr_extract_w(m);
  m[1, 1] ~ normal(sum(w), 1);
}
