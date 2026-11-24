// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"K": 3, "x": [1, 2, 3], "y": [0.5]}
// R writes list(1, 2, 3) as [1, 2, 3], since each element has length 1.
data {
  int K;
  array[K] vector[1] x;
  array[1] vector[1] y;
}
transformed data {
  if (size(x) != 3 || x[1][1] != 1 || x[2][1] != 2 || x[3][1] != 3) reject("x is not [[1], [2], [3]]");
  if (size(y) != 1 || y[1][1] != 0.5) reject("y is not [[0.5]]");
}
parameters { real mu; }
model { mu ~ normal(x[1][1], 1); }
generated quantities {
  real g = x[1][1] + 10 * x[3][1] + 100 * y[1][1];
  if (g != 81) reject("generated quantities read the wrong values");
}
