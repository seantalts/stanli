functions {
  real zs_lpdf(real y, real mu, real s, real A) {
    real d = y - mu;
    real base = d < -s ? -0.5 * square(d / s) - log(s)
                       : log1p_exp(-square(d) * s) - log(s);
    if (A == 0) return base + (1 / A < 0 ? -1.0 : 1.0);
    if (is_nan(A)) return base + 2;
    return base + log(erfc(d * A + 3));
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
  real A;
}
parameters {
  real a;
  real ls;
}
model {
  vector[N] mu = a * x;
  real s = exp(ls);
  for (n in 1:N) {
    real lp = y[n] > mu[n]
      ? zs_lpdf(y[n] | mu[n], s, A)
      : zs_lpdf(y[n] | mu[n] + 0.25, s, A);
    target += lp;
  }
}
