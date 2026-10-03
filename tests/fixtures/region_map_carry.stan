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
  real s = 0;
  vector[N] mu = a * x;
  for (n in 1:N) {
    if (y[n] - mu[n] < b)
      s = s + a;
    else
      s = s + y[n] * b;
  }
  target += -0.01 * square(s);
}
