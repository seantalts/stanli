data {
  int N;
  vector[N] y;
  vector[N] x;
  vector[N] big;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    if (r < b)
      target += big[n] - square(r);
    else
      target += big[n] - 0.5 * square(r) + b;
  }
}
