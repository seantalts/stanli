functions {
  real shifted_lpdf(real y, real mu, real s, real A) {
    real d = y - mu;
    if (A == 0) {
      if (d < -s) return -0.5 * square(d / s) - log(s) - 0.5 * s;
      return log1p_exp(-square(d) * s) - log(s);
    }
    real c = log1p(A) / s;
    return log(erfc(d * c + 3)) - log(A);
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
      ? shifted_lpdf(y[n] | mu[n], s, A)
      : shifted_lpdf(y[n] | mu[n] + 0.25, s, A);
    target += lp;
  }
}
