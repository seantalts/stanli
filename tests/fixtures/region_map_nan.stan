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
    real t;
    if (r < nv || b > nv)
      t = -square(r);
    else
      t = -0.5 * square(r) + b;
    if (!(r >= nv) && r < b) t += 0.1 * a;
    t += (r > nv || r < -b) ? 0.2 * b : -0.1 * r;
    target += t;
  }
}
