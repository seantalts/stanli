functions {
  real guarded_lpdf(real y, real mu, real s) {
    if (s <= 0) return negative_infinity();   // the same test in every row
    return normal_lpdf(y | mu, s);
  }
}
data { int N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> s; }
model {
  vector[N] mu = a + b * x;
  for (n in 1:N) target += guarded_lpdf(y[n] | mu[n], s);
}
