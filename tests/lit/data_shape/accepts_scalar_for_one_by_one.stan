// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"m": 2.5, "a": 3.5, "rv": 4.5}
data {
  matrix[1, 1] m;
  array[1, 1] real a;
  row_vector[1] rv;
}
transformed data {
  if (rows(m) != 1 || cols(m) != 1 || m[1, 1] != 2.5) reject("m is not [[2.5]]");
  if (size(a) != 1 || size(a[1]) != 1 || a[1, 1] != 3.5) reject("a is not [[3.5]]");
  if (cols(rv) != 1 || rv[1] != 4.5) reject("rv is not [4.5]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
