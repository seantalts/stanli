functions {
  real hurdle_lpdf(real y, real mu, real s, real hu) {
    if (y == 0) return log(hu);
    return log1m(hu) + normal_lpdf(y | mu, s);
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
  real hu = inv_logit(h);
  for (n in 1:N) target += hurdle_lpdf(a * y[n] | mu[n], s, hu);
}
