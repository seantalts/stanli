// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real theta;
}
model {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  if (theta > -100)
    target += num_elements(v[:0]) + num_elements(a[:-2])
              + num_elements(v[5:4]) + num_elements(a[5:4]) + sum(v[5:4]);
  theta ~ normal(0, 1);
}
