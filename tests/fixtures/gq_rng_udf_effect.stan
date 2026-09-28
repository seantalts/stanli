functions { real identity_rng(real value) { return value; } }
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real draw = identity_rng(normal_rng(x, 1));
  real after = normal_rng(0, 1);
}
