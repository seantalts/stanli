// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL add: m1 (3, 1) and m2 (4, 1) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 4}
data {
  int N;
  int M;
}
transformed data {
  vector[N] a = rep_vector(1, N);
  vector[M] b = rep_vector(1, M);
  vector[N] c = a + b;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
