// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,g
// STANLI-LIT-DATA: {"s": [7], "t": [[[8]]]}
// R writes array(x, 1) or a draws slice for a scalar. Removing the extents of
// 1 leaves no dims, which is what a scalar declares. CmdStan rejects this.
data {
  real s;
  real t;
}
transformed data {
  if (s != 7 || t != 8) reject("s, t are not 7, 8");
}
parameters { real mu; }
model { mu ~ normal(s, 1); }
generated quantities {
  real g = s + 10 * t;
  if (g != 87) reject("generated quantities read the wrong s or t");
}
