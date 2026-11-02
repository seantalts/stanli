// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL fma: x (3, 1) and y (4, 1) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 4}
data {
  int N;
  int M;
}
transformed data {
  vector[N] a = rep_vector(1, N);
  vector[M] b = rep_vector(1, M);
  vector[N] c = fma(a, b, a);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
