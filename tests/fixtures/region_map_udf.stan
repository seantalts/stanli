functions {
  real tail_lpdf(real y, real mu, real s) {
    real d = y - mu;
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
}
model {
  vector[N] mu = a * x;
  real s = exp(ls);
  for (n in 1:N) target += tail_lpdf(y[n] | mu[n], s);
}
