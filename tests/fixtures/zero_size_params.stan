// Zero-size parameters of rank 1 and 2 beside ordinary ones, for the
// BridgeStan facade's bs_param_unconstrain_json.
parameters {
  real mu;
  matrix[0, 3] m0;
  array[0] vector[2] a0;
  vector[1] b;
}
model {
  mu ~ normal(0, 1);
  b ~ normal(0, 1);
}
