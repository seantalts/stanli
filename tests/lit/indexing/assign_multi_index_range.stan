// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[multi, ...] assign: accessing element out of range. index 5 out of range; expecting index to be between 1 and 4
// STANLI-LIT-DATA: {"N": 4, "M": 3}
data {
  int N;
  int M;
}
transformed data {
  array[N] int c;
  array[M] int d = rep_array(1, M);
  array[M] int is = {1, 5, 2};
  c[is] = d;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
