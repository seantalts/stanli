// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: offset_multiplier_constrain: multiplier[3] is -0.1, but must be positive finite!
// STANLI-LIT-DATA: {"x": [1, 2, 3]}
data {
  vector[3] x;
}
parameters {
  vector[3] s;
}
transformed parameters {
  vector[3] t = offset_multiplier_constrain(x, x, s);
}
model {
  s ~ normal(0, 1);
}
