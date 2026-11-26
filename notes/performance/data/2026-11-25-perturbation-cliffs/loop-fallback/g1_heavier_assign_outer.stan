functions {
  real tail_lpdf(real y, real m, real s) {
    real z = (y - m) / s;
    if (z < -3) return -0.5 * square(z) - log(-z) - log(s);
    if (z > 3) return log1m_exp(-0.5 * square(z)) - log(s);
    if (z < 0) return normal_lpdf(y | m, s) + log1p(-0.01 * z);
    return normal_lpdf(y | m, s);
  }
}
data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = rep_vector(a, N);

  for (n in 1:N) {
    mu[n] = log1p_exp(mu[n]);
    target += tail_lpdf(y[n] | mu[n], s);
  }
}
