data { int<lower=1> N; vector[N] y; }
parameters { simplex[2] theta; ordered[2] mu; real<lower=0> sigma; }
model {
  mu ~ normal(0, 5); sigma ~ normal(0, 2);
  for (n in 1:N) {
    array[2] real ps;
    ps[1] = normal_lpdf(y[n] | mu[1], sigma);
    ps[2] = normal_lpdf(y[n] | mu[2], sigma);
    target += log_mix(theta, ps);
  }
}
