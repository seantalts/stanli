data { int N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> s; }
model {
  vector[N] mu = a + b * x;
  if (s <= 0) target += negative_infinity();
  else for (n in 1:N) target += normal_lpdf(y[n] | mu[n], s);
}
