// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL vector assign rows: assigning variable v (4) and right hand side rows (3) must match in size
// STANLI-LIT-DATA: {"N": 4, "M": 3}
data {
  int N;
  int M;
}
transformed data {
  vector[N] v = rep_vector(0, N);
  vector[M] w = rep_vector(1, M);
  v[:] = w;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
