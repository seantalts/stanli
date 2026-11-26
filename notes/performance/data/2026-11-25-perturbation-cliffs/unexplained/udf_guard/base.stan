data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real alpha; real beta; real<lower=0> sigma; }
model {
  alpha ~ normal(0, 1); beta ~ normal(0, 1); sigma ~ normal(0, 1);
  y ~ normal(alpha + beta * x, sigma);
}
