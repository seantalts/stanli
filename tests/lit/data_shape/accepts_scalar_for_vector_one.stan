// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"v": 7}
// R writes a length-1 vector as a scalar, so stanli accepts a scalar where
// every declared extent is 1. CmdStan rejects this.
data { vector[1] v; }
transformed data {
  if (num_elements(v) != 1 || v[1] != 7) reject("v is not [7]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
