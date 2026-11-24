// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"K": 1, "seasons": 0}
// Issue #671: R sends seasons = 0 as a scalar.
data {
  int<lower=1> K;
  array[K] real<lower=0> seasons;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
