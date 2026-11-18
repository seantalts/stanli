data {
  int<lower=0> N;
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  real g = poisson_log_lpmf(rep_array(1, N) | p);
  real h = poisson_lpmf(rep_array(1, N) | exp(p));
}
