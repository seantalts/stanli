// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real p;
}
transformed parameters {
  // An infinite upper bound makes the transform lower-bounded only, so
  // stan-math never compares the bounds, even though inf < inf is false.
  real t = lower_upper_bound_jacobian(p, positive_infinity(),
                                      positive_infinity());
}
model {
  p ~ normal(0, 1);
}
