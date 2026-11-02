// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Binary function: size of x (2) and size of y (6) must match in size
// STANLI-LIT-DATA: {"N": 2, "M": 6}
data {
  int N;
  int M;
}
transformed data {
  array[N] int y = rep_array(1, N);
  array[M] real p = rep_array(0.5, M);
  array[N] real l = binary_log_loss(y, p);
}
parameters {
  real q;
}
model {
  q ~ normal(0, 1);
}
