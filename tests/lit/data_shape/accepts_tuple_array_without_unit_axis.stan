// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"z": [{"1": 1, "2": [11, 12, 13]}, {"1": 2, "2": [21, 22, 23]}]}
// The outer array[1, 2] is given as a list of two tuples. Each tuple's values
// must still land in its own element.
data { array[1, 2] tuple(real, vector[3]) z; }
transformed data {
  for (j in 1:2) {
    if (z[1, j].1 != j) reject("z[1, j].1 is wrong");
    for (k in 1:3)
      if (z[1, j].2[k] != 10 * j + k) reject("z[1, j].2 is wrong");
  }
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = z[1, 1].2[3] + 100 * z[1, 2].2[1] + 10000 * z[1, 2].1;
  if (g != 22113) reject("generated quantities read the wrong values");
}
