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
  real acc = 0;
  vector[N] mu = a * x;
  for (n in 1:N) {
    real r = y[n] - mu[n];
    if (acc > 1e30 || r < b)
      acc += -square(r);
    else
      acc += -0.5 * square(r) + b;
  }
  target += acc;
}
