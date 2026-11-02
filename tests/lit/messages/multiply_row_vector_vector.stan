// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL multiply: Columns of rv (3) and Rows of v (2) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 2}
data {
  int N;
  int M;
}
transformed data {
  row_vector[N] a = rep_row_vector(1, N);
  vector[M] b = rep_vector(1, M);
  real c = a * b;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
