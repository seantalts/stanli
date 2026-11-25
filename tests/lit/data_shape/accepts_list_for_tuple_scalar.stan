// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"tp": {"1": [1.5], "2": [1, 2]}}
data { tuple(real, array[2] int) tp; }
transformed data {
  if (tp.1 != 1.5 || tp.2[2] != 2) reject("tp is not (1.5, [1, 2])");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real g = tp.1 + 10 * tp.2[2];
  if (g != 21.5) reject("generated quantities read the wrong tp");
}
