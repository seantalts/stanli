// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign columns: assigning variable r (3) and right hand side columns (2) must match in size
transformed data {
  row_vector[3] r = [1, 2];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
