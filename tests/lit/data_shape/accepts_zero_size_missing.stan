// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {}
// A variable with a zero extent may be left out.
data {
  matrix[0, 3] Z;
  array[0] vector[3] A0;
  array[2] vector[0] A2;
  vector[0] v0;
  array[0] int i0;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
