// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: lub_constrain: lb is 2, but must be less than 2.000000
// STANLI-LIT-DATA: {"b": 2}
data {
  real b;
}
parameters {
  real p;
}
transformed parameters {
  real t = lower_upper_bound_jacobian(p, b, b);
}
model {
  p ~ normal(0, 1);
}
