functions {
  real shift(real y, real m) { return y - m; }
  real tail2_lpdf(real y, real m, real s) {
    real d = 0;
    d = shift(y, m);
    if (d < -s) return -0.5 * square(d / s) - log(s) - 0.5 * s;
    return log1p_exp(-square(d) * s) - log(s);
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
  real h;
}
model {
  vector[N] mu = a * x;
  real s = exp(ls);
  for (n in 1:N) target += tail2_lpdf(y[n] | mu[n], s) + h * 0;
}
