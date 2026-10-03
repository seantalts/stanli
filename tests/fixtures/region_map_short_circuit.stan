data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    if (b <= 0 || normal_lpdf(y[n] | a * x[n], b) < -5)
      target += -0.1 * square(r) + b;
    else
      target += -0.5 * square(r) + a;
  }
}
