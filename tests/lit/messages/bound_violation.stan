// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL k is -1, but must be greater than or equal to 0
// STANLI-LIT-DATA: {"k": -1}
data {
  real<lower=0> k;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
