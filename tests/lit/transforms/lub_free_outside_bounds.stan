// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: lub_free: Bounded variable is
parameters {
  real p;
}
transformed parameters {
  real t = lower_upper_bound_unconstrain(1 + exp(p), 0, 1);
}
model {
  p ~ normal(0, 1);
}
