parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities { array[2] real draw = gamma_rng(rep_vector(-1, 2), 1.5); }
