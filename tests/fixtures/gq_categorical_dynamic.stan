data {
  int<lower=1> K;
}
parameters {
  vector[K] p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  int draw = categorical_rng(p);
  real picked = p[draw];
}
