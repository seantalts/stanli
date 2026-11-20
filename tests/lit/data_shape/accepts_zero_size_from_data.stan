// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 0, "y": 1.5, "z": [[]]}
// An extent that comes from other data counts the same as a literal zero.
data {
  int N;
  vector[N] y;
  array[N] vector[2] z;
}
transformed data {
  if (num_elements(y) != 0 || size(z) != 0) reject("zero-size data is not empty");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
