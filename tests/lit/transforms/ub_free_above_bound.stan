// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: ub_free: Upper bounded variable is
parameters {
  real p;
}
transformed parameters {
  real t = upper_bound_unconstrain(exp(p), 0);
}
model {
  p ~ normal(0, 1);
}
