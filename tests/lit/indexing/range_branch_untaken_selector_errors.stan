// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real theta;
}
model {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  if (theta > 100) {
    target += sum(v[4:]) + sum(v[2:5]) + sum(a[5:]);
    v[4:] = v[1:0];
  }
  theta ~ normal(0, 1);
}
