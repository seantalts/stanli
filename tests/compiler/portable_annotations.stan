data {
  int<lower=1> J;
  vector[J] y;
}
parameters {
  @latent vector[J] a;
  real mu;
  real<lower=0> sigma;
}
model {
  a ~ normal(mu, sigma);
  y ~ normal(a, 1);
}
