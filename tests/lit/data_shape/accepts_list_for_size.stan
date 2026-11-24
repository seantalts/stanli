// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"N": [3], "y": [1, 2, 3]}
// A size given as [3] is read as 3, so it can size later declarations.
data {
  int N;
  vector[N] y;
}
transformed data {
  if (N != 3 || num_elements(y) != 3 || y[3] != 3) reject("N is not 3");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = N + sum(y);
  if (g != 9) reject("generated quantities read the wrong N");
}
