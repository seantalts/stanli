data {
  int<lower=0> K;
  array[K] int trials;
}
parameters {
  vector<lower=0, upper=1>[K] p;
  real mu;
  real<lower=0> sigma;
}
transformed parameters {
  real tp_val = mu + sigma;
}
generated quantities {
  real ok_val = tp_val * 2;
  array[K] int draws = binomial_rng(trials, p);
}
