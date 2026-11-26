data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  y ~ normal(a, s);
  for (m in 1:1000) {              // every iteration is the same

    target += cauchy_lpdf(s | 0, 2.5) / 1000;
  }
}
