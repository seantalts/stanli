functions {
  real inner(real d, real s, real A) {
    if (A == 0) {
      if (d < -s) return -0.5 * square(d / s) - log(s) - 0.5 * s;
      return log1p_exp(-square(d) * s) - log(s);
    }
    return log(erfc(d * A + 3));
  }
  real middle(real d, real s, real A) { return inner(d, s, A); }
  real outer_lpdf(real y, real mu, real s, real A) {
    return middle(y - mu, s, A);
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
  real A = 0;
  for (n in 1:N) {
    real lp = y[n] > mu[n]
      ? outer_lpdf(y[n] | mu[n], s, A)
      : outer_lpdf(y[n] | mu[n] + 0.25, s, A);
    target += lp;
  }
}
