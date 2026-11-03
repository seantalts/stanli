// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL d[2] is not a valid row stochastic matrix
// STANLI-LIT-DATA: {"d": [[[0.5, 0.5], [0.5, 0.5]], [[0.5, 0.5], [0.6, 0.5]]]}
data {
  array[2] row_stochastic_matrix[2, 2] d;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
