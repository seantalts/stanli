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
    real yn = y[n];
    real mun = mu[n];
    real d = yn - mun;
    real lp;
    if (d < -s)
      lp = -0.5 * square(d / s) - log(s) - 0.5 * s;
    else
      lp = log1p_exp(-square(d) * s) - log(s);
    target += lp;
  }
}
