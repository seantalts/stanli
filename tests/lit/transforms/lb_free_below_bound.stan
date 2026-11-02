// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: lb_free: Lower bounded variable is
parameters {
  vector[2] p;
}
transformed parameters {
  vector[2] t = lower_bound_unconstrain(-exp(p), 0);
}
model {
  p ~ normal(0, 1);
}
