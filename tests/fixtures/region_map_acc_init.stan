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
  real acc = 2.5;
  vector[N] mu = a * x;
  for (n in 1:N) {
    real r = y[n] - mu[n];
    if (r < b)
      acc += -square(r) - log1p(square(b));
    else
      acc += -0.5 * square(r) + b;
  }
  target += -0.01 * square(acc);
}
