data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  y ~ normal(a, s);
  for (m in 1:1000) {              // every iteration is the same
    if (is_nan(s) || s <= 0) reject("s must be positive, got ", s);
    target += cauchy_lpdf(s | 0, 2.5) / 1000;
  }
}
