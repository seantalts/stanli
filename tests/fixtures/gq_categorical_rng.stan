data {
  int<lower=0> K;
}
parameters {
  vector[K] p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  int<lower=1, upper=K> draw = categorical_rng(p);
  real tail = uniform_rng(0, 1);
}
