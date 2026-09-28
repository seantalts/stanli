transformed data { real shape = 2; }
parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities { array[2] real draw = gamma_rng(rep_vector(rate, 2), 1.5); }
