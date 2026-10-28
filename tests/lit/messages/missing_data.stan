// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL missing data for k; the data must give every variable in the data block
data {
  int k;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
