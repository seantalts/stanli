parameters {
  real x;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  real before = normal_rng(x, 1);
  array[3] real batch = normal_rng(rep_vector(x, 3), 0.7);
  real after = normal_rng(x, 1);
}
