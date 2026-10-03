data {
  int<lower=1> N;
  vector[N] y;
}
parameters {
  vector[3] th;
  real g;
}
model {
  for (n in 1 : N) {
    if (g * y[n] > 0) {
      int k = (th[1] > th[2]) + 1;
      target += normal_lpdf(y[n] | th[k], 1);
    } else {
      int k = (th[2] > th[3]) + 2;
      target += normal_lpdf(y[n] | th[k], 2);
    }
  }
  target += -0.5 * dot_self(th) - 0.5 * square(g);
}
