// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL multiply: Columns of m1 (3) and Rows of m2 (2) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 2}
data {
  int N;
  int M;
}
transformed data {
  matrix[1, N] a = rep_matrix(1, 1, N);
  matrix[M, N] b = rep_matrix(1, M, N);
  matrix[1, N] c = a * b;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
