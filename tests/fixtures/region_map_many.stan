data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
  real c;
  real d;
}
model {
  vector[N] v1 = a * x;
  vector[N] v2 = b * x + 0.3;
  vector[N] v3 = c * x - 0.2;
  vector[N] v4 = d * square(x);
  for (n in 1:N) {
    real r = y[n] - v1[n];
    if (r < v2[n])
      target += -square(v3[n] - y[n]) - v4[n] * x[n];
    else
      target += -0.5 * square(r) + v4[n] - 0.1 * v2[n] * v3[n];
  }
}
