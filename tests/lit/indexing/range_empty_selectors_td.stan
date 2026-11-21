// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  int n = num_elements(v[:0]) + num_elements(a[:-2]) + num_elements(v[5:4])
          + num_elements(a[5:4]);
  if (n != 0) reject("empty selectors selected ", n);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
