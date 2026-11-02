// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign rows: assigning variable td (8) and right hand side rows (7) must match in size
// STANLI-LIT-DATA: {"b0": [1, 2, 3, 4, 5, 6, 7]}
data {
  vector[7] b0;
}
transformed data {
  vector[8] td = b0;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
