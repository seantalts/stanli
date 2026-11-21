// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"a": [1, 2, 3]}
data {
  array[3] real a;
}
transformed data {
  array[0] real e = a[4:];
  array[2, 3] real a2 = {a, a};
  array[0] real f = a2[1, 4:];
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
