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
    if (r < b) {
      int k = 2;
      target += -k * square(r);
    } else {
      int k = 1;
      target += -0.5 * k * square(r) + b;
    }
  }
}
