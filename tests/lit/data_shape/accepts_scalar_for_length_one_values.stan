// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: mu,s
// STANLI-LIT-DATA: {"K": 1, "seasons": 2.5, "tp": {"1": 1.5, "2": 3}}
// The scalar is bound as a one-element array, so transformed data, the model
// and generated quantities all read it.
data {
  int<lower=1> K;
  array[K] real<lower=0> seasons;
  tuple(real, array[1] real) tp;
}
transformed data {
  real first = seasons[1];
  array[K] real doubled;
  for (k in 1:K) doubled[k] = 2 * seasons[k];
  if (first != 2.5 || size(seasons) != 1 || doubled[1] != 5) reject("seasons is not [2.5]");
  if (size(tp.2) != 1 || tp.2[1] != 3) reject("tp.2 is not [3]");
}
parameters { real mu; }
model { mu ~ normal(seasons[1], 1); }
generated quantities {
  real s = seasons[1] + sum(doubled) + tp.2[1];
  if (s != 10.5) reject("generated quantities read the wrong seasons");
}
