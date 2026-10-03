data {
  int N;
  vector[N] y;
  vector[N] x;
  real nv;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    if (r < nv || b > nv)
      target += -square(r);
    else
      target += -0.5 * square(r) + b;
    if (!(r >= nv) && r < b) target += 0.1 * a;
    target += (r > nv || r < -b) ? 0.2 * b : -0.1 * r;
  }
}
