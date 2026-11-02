// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL array[uni,...] assign: accessing element out of range. index 5 out of range; expecting index to be between 1 and 4
// STANLI-LIT-DATA: {"N": 4, "k": 5}
data {
  int N;
  int k;
}
transformed data {
  array[N] int c;
  c[k] = 1;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
