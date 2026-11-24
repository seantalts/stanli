// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"ai": 3}
data { array[1] int ai; }
transformed data {
  if (size(ai) != 1 || ai[1] != 3) reject("ai is not [3]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
