data { int N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> s; }
model {
  vector[N] mu;
  for (i in 1:N) {
    int n = i;
    mu[n] = a + b * x[n];
  }
  y ~ normal(mu, s);
}
