// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"m": [1.5], "av": [2.5], "a": [3.5]}
// R writes array(x, 1) for these. Removing the extents of 1 gives the same
// empty list of dims on both sides.
data {
  matrix[1, 1] m;
  array[1] vector[1] av;
  array[1, 1] real a;
}
transformed data {
  if (rows(m) != 1 || cols(m) != 1 || m[1, 1] != 1.5) reject("m is not [[1.5]]");
  if (size(av) != 1 || num_elements(av[1]) != 1 || av[1][1] != 2.5) reject("av is not [[2.5]]");
  if (size(a) != 1 || size(a[1]) != 1 || a[1, 1] != 3.5) reject("a is not [[3.5]]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = m[1, 1] + 10 * av[1][1] + 100 * a[1, 1];
  if (g != 376.5) reject("generated quantities read the wrong values");
}
