// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL v[2][3] is -1, but must be greater than or equal to 0.000000
transformed data {
  array[2] vector<lower=0>[3] v = {[1, 1, 1]', [1, 1, -1]'};
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
