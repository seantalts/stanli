functions {
  real score_lpdf(real y, real x, real a, real b) {
    if (b <= -5 || a > 6 || a < -6 || y > 40 || x > 40) return negative_infinity();
    real r = y - a * x;
    real s = exp(b);
    real w = (r < -s || r > s) ? -0.5 * square(r / s) - b : -square(r) * s;
    if (r > 0 && b > 0) w += 0.1 * a;
    return w;
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) target += score_lpdf(y[n] | x[n], a, b);
}
