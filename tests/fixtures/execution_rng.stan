transformed data { real shape = 2; }
parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities { real draw = gamma_rng(shape, rate); }
