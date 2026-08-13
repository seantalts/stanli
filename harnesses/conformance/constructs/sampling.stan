parameters {
  real theta;
  real<lower=0> sigma;
}
model {
  theta ~ normal(0, sigma) T[-1, 2];
  target += 0.125 * theta;
}
