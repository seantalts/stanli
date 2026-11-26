data { int<lower=0> N; vector[N] y; }
parameters { real mu; real<lower=0> sigma; }
model {
  mu ~ normal(0, 1); sigma ~ normal(1, 0.5);
  for (n in 1:N) target += normal_lccdf(y[n] | mu, sigma);
}
