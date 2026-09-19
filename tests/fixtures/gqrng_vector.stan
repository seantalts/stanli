data {
  int<lower=0> K;
}
parameters {
  vector[K] mu;
  real<lower=0> sigma;
}
generated quantities {
  array[K] real draws_vv = normal_rng(mu, rep_vector(sigma, K));
  array[K] real draws_vr = normal_rng(mu, sigma);
}
