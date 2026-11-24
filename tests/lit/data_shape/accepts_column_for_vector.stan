// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"v": [[1], [2], [3]], "w": [[4, 5, 6]], "ar": [[7], [8], [9]], "ar2": [[10, 11, 12]]}
// R's scale(x) and X %*% b give an N x 1 matrix, and a 1 x N matrix comes
// from t(x). Removing the extent of 1 leaves the declared (3), and the values
// keep their order.
data {
  vector[3] v;
  row_vector[3] w;
  array[3] real ar;
  array[3] real ar2;
}
transformed data {
  if (v[1] != 1 || v[2] != 2 || v[3] != 3) reject("v is not [1, 2, 3]");
  if (w[1] != 4 || w[2] != 5 || w[3] != 6) reject("w is not [4, 5, 6]");
  if (ar[1] != 7 || ar[2] != 8 || ar[3] != 9) reject("ar is not [7, 8, 9]");
  if (ar2[1] != 10 || ar2[2] != 11 || ar2[3] != 12) reject("ar2 is not [10, 11, 12]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = v[1] + 10 * v[3] + 100 * w[3] + 1000 * ar[2] + 10000 * ar2[1];
  if (g != 108631) reject("generated quantities read the wrong values");
}
