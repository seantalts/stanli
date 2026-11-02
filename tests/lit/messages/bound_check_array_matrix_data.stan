// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL a[2, 1][1, 2] is -1, but must be greater than or equal to 0.000000
transformed data {
  array[2, 2] matrix<lower=0>[2, 2] a;
  for (i in 1:2) {
    for (j in 1:2) {
      a[i, j] = rep_matrix(1, 2, 2);
    }
  }
  a[2, 1][1, 2] = -1;
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
