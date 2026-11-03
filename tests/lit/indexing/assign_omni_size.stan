// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL assign array size: assigning variable c (4) and right hand side (3) must match in size
// STANLI-LIT-DATA: {"N": 4, "M": 3}
data {
  int N;
  int M;
}
transformed data {
  array[N] int c;
  array[M] int d = rep_array(1, M);
  c[:] = d;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
