functions {
  real guarded_lpdf(real y, real mu, real s) {
    if (s <= 0) return negative_infinity();
    return normal_lpdf(y | mu, s);
  }
}
data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real alpha; real beta; real<lower=0> sigma; }
model {
  alpha ~ normal(0, 1); beta ~ normal(0, 1); sigma ~ normal(0, 1);
  for (n in 1:N) target += guarded_lpdf(y[n] | alpha + beta * x[n], sigma);
}
