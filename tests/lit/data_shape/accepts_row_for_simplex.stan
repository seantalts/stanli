// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"x": [[0.2, 0.3, 0.5]]}
data { simplex[3] x; }
transformed data {
  if (x[1] != 0.2 || x[3] != 0.5) reject("x is not [0.2, 0.3, 0.5]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = x[3];
  if (g != 0.5) reject("generated quantities read the wrong x");
}
