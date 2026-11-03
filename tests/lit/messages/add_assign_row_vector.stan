// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL add: m1 (1, 3) and m2 (1, 4) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 4}
data {
  int N;
  int M;
}
transformed data {
  row_vector[N] a = rep_row_vector(1, N);
  row_vector[M] b = rep_row_vector(1, M);
  a += b;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
