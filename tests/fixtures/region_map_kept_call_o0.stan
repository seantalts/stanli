functions {
  real tail_term(real y, real mu, real s) {
    real d = y - mu;
    real lp;
    if (d < -s)
      lp = -0.5 * square(d / s) - log(s) - 0.5 * s;
    else
      lp = log1p_exp(-square(d) * s) - log(s);
    return lp;
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
  for (n in 1:N) target += tail_term(y[n], mu[n], s);
}
