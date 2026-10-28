// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL complex numbers are not supported yet (in variable z)
// STANLI-LIT-DATA: {"z": [1, 2]}
data {
  complex z;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
