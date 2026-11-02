// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: offset_multiplier_constrain: multiplier is -0.1, but must be positive finite!
parameters {
  real p;
}
transformed parameters {
  real t = offset_multiplier_jacobian(p, 0, -p);
}
model {
  p ~ normal(0, 1);
}
