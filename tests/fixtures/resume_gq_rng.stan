data {
  int<lower=1> N;
  vector[N] y;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 5);
  sigma ~ lognormal(0, 1);
  y ~ normal(mu, sigma);
}
generated quantities {
  real y_rep = normal_rng(mu, sigma);
  int k = poisson_rng(3);
}
