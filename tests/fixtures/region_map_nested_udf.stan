functions {
  real residual(real y, real m) { return y - m; }
  real scaled(real y, real m, real s) {
    real d = residual(y, m);
    return d / s;
  }
  real score(real y, real m, real s) {
    real z = scaled(y, m, s);
    return -0.5 * square(z) - log(s);
  }
  real bump(real y, real m, real s) {
    real z = scaled(y, m, s);
    return log1p_exp(-square(z) * s) - log(s);
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
  for (n in 1:N) {
    if (y[n] - mu[n] < -s)
      target += score(y[n], mu[n], s);
    else
      target += bump(y[n], mu[n], s);
  }
}
