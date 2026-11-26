// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: offset_multiplier_constrain: multiplier[2] is -1, but must be positive finite!
// Stan Math checks a vector multiplier whole, so the message names the
// element.
transformed data {
  vector[3] x = [1, 2, 3]';
  vector[3] y = offset_multiplier_constrain(x, rep_vector(0, 3), [1, -1, 2]');
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
}
