functions {
  real nn_lpdf(real y, real mu, real s, real A) {
    real d = y - mu;
    real base = d < -s ? -0.5 * square(d / s) - log(s)
                       : log1p_exp(-square(d) * s) - log(s);
    if (A == 0) return base + log(erfc(d + 3));
    return base + (A != 0 ? 3.0 : 4.0);
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real ls;
}
model {
  vector[N] mu = a * x;
  real s = exp(ls);
  real A = not_a_number();
  for (n in 1:N) {
    real lp = y[n] > mu[n]
      ? nn_lpdf(y[n] | mu[n], s, A)
      : nn_lpdf(y[n] | mu[n] + 0.25, s, A);
    target += lp;
  }
}
