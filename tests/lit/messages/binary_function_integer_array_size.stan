// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Binary function: size of x (3) and size of y (2) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 2}
data {
  int N;
  int M;
}
transformed data {
  vector[N] a = rep_vector(1, N);
  array[M] int b = rep_array(1, M);
  vector[N] c = ldexp(a, b);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
