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
  target += normal_lpdf(a | 0, 2);
  for (n in 1:N) {
    real r = y[n] - mu[n];
    target += -0.1 * square(b);
    if (r < b)
      target += -square(r);
    else
      target += -0.5 * square(r) + b;
    target += -0.01 * r;
  }
  target += normal_lpdf(b | 0, 3);
}
