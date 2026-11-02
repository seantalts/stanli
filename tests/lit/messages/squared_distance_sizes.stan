// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL squared_distance: size of v1 (3) and size of v2 (4) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 4}
data {
  int N;
  int M;
}
transformed data {
  vector[N] a = rep_vector(1, N);
  vector[M] b = rep_vector(1, M);
  real z = squared_distance(a, b);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
