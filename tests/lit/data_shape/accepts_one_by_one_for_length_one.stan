// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"v": [[1.5]], "rv": [[2.5]], "a": [[3.5]]}
// R's X %*% b with one row, or as.matrix() of one cell, gives a 1 x 1 matrix.
data {
  vector[1] v;
  row_vector[1] rv;
  array[1] real a;
}
transformed data {
  if (num_elements(v) != 1 || v[1] != 1.5) reject("v is not [1.5]");
  if (num_elements(rv) != 1 || rv[1] != 2.5) reject("rv is not [2.5]");
  if (size(a) != 1 || a[1] != 3.5) reject("a is not [3.5]");
}
parameters { real mu; }
model { mu ~ normal(v[1], 1); }
generated quantities {
  real g = v[1] + 10 * rv[1] + 100 * a[1];
  if (g != 376.5) reject("generated quantities read the wrong values");
}
