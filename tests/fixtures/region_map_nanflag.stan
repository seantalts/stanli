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
    real d = a * x[n] - y[n];
    if (d > 0)
      target += -abs(log(d)) * b;
    else
      target += -0.5 * square(d);
  }
}
