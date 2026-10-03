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
  vector[N] nu = x * a;
  for (n in 1:N) {
    real d = mu[n] - y[n];
    real e = nu[n] - 0.5 * y[n];
    if (d < b)
      target += -square(d) + e;
    else
      target += -0.5 * square(e) + b;
  }
}
