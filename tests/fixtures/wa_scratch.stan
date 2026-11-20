data {
  int<lower=1> N;
  array[N] int<lower=0> y;
  vector[N] x;
}
parameters {
  real mu;
  real<lower=0> phi;
}
model {
  mu ~ normal(0, 1);
  phi ~ gamma(2, 1);
}
generated quantities {
  real lp_a = neg_binomial_2_log_lpmf(y | mu + x, phi);
  real lp_b = neg_binomial_2_log_lpmf(y | mu - x, phi + 1);
  real lse = log_sum_exp(mu * x);
}
