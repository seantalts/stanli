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
    real d = y[n] - mu[n];
    real acc = 0;
    if (d < -s) {
      for (k in 1:4) {
        if (acc + k * s > 1.5) break;
        if (k == 2) continue;
        acc += k * s;
      }
      target += -0.5 * square(d / s) - acc;
    } else {
      target += log1p_exp(-square(d) * s) - log(s);
    }
  }
}
