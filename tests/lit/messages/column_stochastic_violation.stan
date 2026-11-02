// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL d is not a valid column stochastic matrix
// STANLI-LIT-DATA: {"d": [[0.5, 0.5], [0.6, 0.5]]}
data {
  column_stochastic_matrix[2, 2] d;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
