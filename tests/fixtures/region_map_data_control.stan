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
  vector[N] mu = a * x;
  for (n in 1:N) {
    real r = y[n] - mu[n];
    if (y[n] < 0.1)
      target += -square(r) + b;
    else
      target += -0.5 * square(r) - b;
  }
}
