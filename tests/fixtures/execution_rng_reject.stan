parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities { real draw = poisson_binomial_rng(rep_vector(-1, 2)); }
