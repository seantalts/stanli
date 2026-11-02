// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Found negative dimension size in variable declaration; variable=arr; dimension size expression=N; expression value=-2
// STANLI-LIT-DATA: {"N": -2}
data {
  int N;
}
transformed data {
  array[N, N] real arr;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
