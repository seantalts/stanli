// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"atp": [{"1": [[1.0, 2.0]], "2": 3}, {"1": [[4.0, 5.0]], "2": 6}]}
// Each vector[2] is given as a 1 x 2 matrix. Removing the extent of 1 leaves
// the declared (2, 2).
data { array[2] tuple(vector[2], int) atp; }
transformed data {
  if (atp[1].1[1] != 1 || atp[1].1[2] != 2 || atp[2].1[1] != 4 || atp[2].1[2] != 5
      || atp[1].2 != 3 || atp[2].2 != 6)
    reject("atp has the wrong values");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = atp[1].1[2] + 10 * atp[2].1[1] + 100 * atp[2].2;
  if (g != 642) reject("generated quantities read the wrong atp");
}
