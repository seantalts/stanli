// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL quad_form: Columns of A (2) and Rows of B (3) must match in size
// STANLI-LIT-DATA: {"N": 2, "M": 3}
data {
  int N;
  int M;
}
transformed data {
  matrix[N, N] a = identity_matrix(N);
  matrix[M, N] b = rep_matrix(1, M, N);
  matrix[N, N] c = quad_form(a, b);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
