parameters {
  real x;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  int p = poisson_log_rng(0.3);
  real u = uniform_rng(-2, 3);
  int b = bernoulli_rng(0.4);
  real n = normal_rng(x, 1.2);
  real l = lognormal_rng(0.2, 0.7);
}
