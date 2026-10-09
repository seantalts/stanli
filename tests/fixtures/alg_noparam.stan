// No parameters at all: the model fixed_param exists for.
data {
  real loc;
}
generated quantities {
  real y = normal_rng(loc, 2);
  int k = poisson_rng(3);
}
