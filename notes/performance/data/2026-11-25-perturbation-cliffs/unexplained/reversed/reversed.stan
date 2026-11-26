data { int<lower=1> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model {
  vector[N] mu = rep_vector(a, N);
  for (i in 1:N) { int n = N - i + 1; mu[n] += b * x[n]; }
  y ~ normal(mu, sigma);
}
