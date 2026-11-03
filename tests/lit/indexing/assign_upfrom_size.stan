// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[multi, ...] assign: assigning variable e (4) and right hand side size (3) must match in size
// STANLI-LIT-DATA: {"N": 5, "M": 3}
data {
  int N;
  int M;
}
transformed data {
  array[N] int e;
  array[M] int f = rep_array(1, M);
  e[2:] = f;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
