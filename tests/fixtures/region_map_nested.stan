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
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    real w;
    if (r > 0) {
      if (b > 0) {
        w = -0.5 * square(r) * b;
        if (a > x[n])
          w += 0.2 * a;
        else
          w -= 0.1 * b * r;
      } else {
        w = -abs(r) - 0.3 * a;
      }
      w += 0.05 * r;
    } else {
      if (a * b > x[n])
        w = -square(r) + b;
      else
        w = -0.5 * r * r - a * x[n];
    }
    target += w;
  }
}
