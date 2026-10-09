data { int<lower=1> N; array[N] int<lower=0> y; array[N] int<lower=0> T; vector[N] x; }
parameters { real<lower=0,upper=1> w; real a0; real a1; real<lower=0> phi; }
model { a0 ~ normal(0,1); a1 ~ normal(0,1); phi ~ exponential(0.1);
 for (n in 1:N) target += 0.5 * beta_binomial_lpmf(y[n] | T[n], exp(a0 + a1 * x[n]), phi); }
