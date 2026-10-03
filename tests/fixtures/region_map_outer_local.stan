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
  real r;
  vector[N] mu = a * x;
  for (n in 1:N) {
    r = y[n] - mu[n];
    if (r < b)
      target += -square(r);
    else
      target += -0.5 * square(r) + b;
  }
}
