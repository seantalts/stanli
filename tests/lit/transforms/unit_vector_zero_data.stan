// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL unit_vector_constrain: norm is 0, but must be positive finite
// STANLI-LIT-DATA: {"d": [0, 0]}
// A data argument takes the double overload, which checks the norm.
data {
  vector[2] d;
}
parameters {
  real p;
}
transformed parameters {
  vector[2] u = unit_vector_constrain(d);
}
model {
  p ~ normal(0, 1);
}
