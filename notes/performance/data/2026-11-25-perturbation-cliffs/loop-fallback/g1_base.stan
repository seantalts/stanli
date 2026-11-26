data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = rep_vector(a, N);
  mu = exp(mu);
  for (n in 1:N) {
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
}
