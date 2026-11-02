// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL categorical_logit_glm_lpmf: Intercept vector has dimension = 2, expecting dimension = 3; a function was called with arguments of different scalar, array, vector, or matrix types, and they were not consistently sized;  all arguments must be scalars or multidimensional values of the same shape.
transformed data {
  matrix[2, 3] x = [[1, 2, 3], [4, 5, 6]];
  matrix[2, 3] beta = [[1, 2, 3], [4, 5, 6]];
  vector[2] alpha = [0.5, 0.6]';
  real lp = categorical_logit_glm_lpmf(1 | x, alpha, beta);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
