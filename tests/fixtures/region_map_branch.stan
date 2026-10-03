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
    if (r < b)
      target += -square(r) - log1p(square(b));
    else
      target += -0.5 * square(r) + b;
  }
}
