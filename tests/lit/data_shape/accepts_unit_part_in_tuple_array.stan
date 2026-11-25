// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"t": [{"1": 5, "2": 1}, {"1": 7, "2": 2}], "u": [{"1": 1.5, "2": 3}]}
// R writes each vector[1] part as a scalar.
data {
  array[2] tuple(vector[1], real) t;
  array[1] tuple(vector[1], real) u;
}
transformed data {
  if (t[1].1[1] != 5 || t[2].1[1] != 7 || t[1].2 != 1 || t[2].2 != 2) reject("t has the wrong values");
  if (u[1].1[1] != 1.5 || u[1].2 != 3) reject("u has the wrong values");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = t[2].1[1] + 10 * t[2].2 + 100 * u[1].1[1];
  if (g != 177) reject("generated quantities read the wrong values");
}
