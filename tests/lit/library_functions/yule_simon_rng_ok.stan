// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters { real<lower=0.5> a; }
model { a ~ normal(1, 1); }
generated quantities {
  int draw = yule_simon_rng(a);
  if (draw < 1) reject("yule_simon_rng support");
}
