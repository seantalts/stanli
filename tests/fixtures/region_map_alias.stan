functions {
  real pair_lpdf(real y, real m1, real m2, real s) {
    real d = y - m1;
    if (d < s) return -square(d) - 0.5 * square(m2) * s;
    return -0.5 * square(y - m2) + s;
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real s;
}
model {
  vector[N] mu = a * x;
  target += -0.01 * dot_self(mu);
  for (n in 1:N) target += pair_lpdf(y[n] | mu[n], mu[n], s);
  target += normal_lpdf(mu | 0, 5);
}
