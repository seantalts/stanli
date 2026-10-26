// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL
// STANLI-LIT-DATA: {"N": 3}
data {
  int N;
}
parameters {
  vector[N] mu;
}
model {
  int k = 0;
  while (k >= 0) k += 1;
  vector[k] z = mu[1 : 1];
  z ~ normal(0, 1);
}
