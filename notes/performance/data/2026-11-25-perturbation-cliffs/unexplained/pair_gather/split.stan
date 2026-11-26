data { int<lower=0> N; int<lower=0> J; array[N] int<lower=1, upper=J> g; vector[N] x; vector[N] y; }
parameters { vector[J] a; vector[J] b; real<lower=0> sigma; }
model {
  a ~ normal(0, 1); b ~ normal(0, 1); sigma ~ normal(0, 1);
  vector[N] mu;
  for (n in 1:N) mu[n] = a[g[n]] + x[n] * b[g[n]];
  y ~ normal(mu, sigma);
}
