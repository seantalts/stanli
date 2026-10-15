data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
  real c;
}
model {
  for (n in 1:N) {
    real u = y[n] - a * x[n];
    real v;
    if (u < b)
      v = c * square(u);
    else
      v = exp(-u) - b;
    real w;
    if (v < c)
      w = log1p(square(v));
    else
      w = a * v;
    target += -w - 0.5 * square(u);
  }
}
