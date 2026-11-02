// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL subtract: m1 (2, 3) and m2 (2, 2) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 2}
data {
  int N;
  int M;
}
transformed data {
  matrix[2, N] a = rep_matrix(1, 2, N);
  matrix[2, M] b = rep_matrix(1, 2, M);
  matrix[2, N] c = a - b;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
