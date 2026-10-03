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
    if (r < b)
      target += -log1p(r);
    else
      target += -0.5 * square(r) + b;
  }
}
