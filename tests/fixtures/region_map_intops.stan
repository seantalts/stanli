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
    int k = n % 3;
    int h = n / 2;
    if (r < b)
      target += -(k + 1) * square(r) - 0.01 * h * b;
    else
      target += -0.5 * square(r) + b * (h % 2);
  }
}
