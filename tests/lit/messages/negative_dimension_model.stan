// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Found negative dimension size in variable declaration; variable=a; dimension size expression=N; expression value=-2
// STANLI-LIT-DATA: {"N": -2}
data {
  int N;
}
parameters {
  real y;
}
model {
  array[N] real a;
  y ~ normal(0, 1);
}
