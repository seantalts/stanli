data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 3);
  sigma ~ exponential(1);
  y ~ normal(mu, sigma);
}
generated quantities {
  // One loop of draws. poisson_rng and gamma_rng take a varying number of
  // uniforms per draw, a parameter branch chooses whether to draw, and
  // level carries a value from one iteration to the next.
  vector[N] y_rep;
  array[N] int count;
  vector[N] shape;
  vector[N] jump;
  real level = 0;
  for (n in 1:N) {
    y_rep[n] = normal_rng(mu + level, sigma);
    count[n] = poisson_rng(exp(3 + x[n]) * 30);
    shape[n] = gamma_rng(0.3 + sigma, 1);
    if (mu > x[n]) {
      jump[n] = exponential_rng(1);
    } else {
      jump[n] = -1;
    }
    level = 0.5 * level + x[n] * mu;
  }
  real last = normal_rng(level, 1);
}
