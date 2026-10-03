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
    real acc = 0;
    for (j in 1:340)
      acc += exp(-0.002 * j * square(r)) * b;
    if (r < b)
      target += -square(r) + 0.001 * acc;
    else
      target += -0.5 * square(r) + b - 0.002 * acc;
  }
}
