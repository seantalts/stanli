// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real theta;
}
model {
  array[3] real a = {1, 2, 3};
  array[0] real e;
  if (theta > -100) {
    target += num_elements(a[4:]);
    a[4:] = e;
  }
  theta ~ normal(0, 1);
}
