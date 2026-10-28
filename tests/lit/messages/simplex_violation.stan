// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL s is not a valid simplex
// STANLI-LIT-DATA: {"s": [0.5, 0.5, 0.5]}
data {
  simplex[3] s;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
