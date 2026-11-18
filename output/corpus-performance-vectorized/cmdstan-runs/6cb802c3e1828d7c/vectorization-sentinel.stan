data { int<lower=1> N; vector[N] y; }
parameters { real mu; real<lower=0> sigma; }
model { for (n in 1:N) y[n] ~ normal(mu, sigma); }
