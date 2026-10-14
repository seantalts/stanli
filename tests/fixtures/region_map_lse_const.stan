functions {
  real mixed_lpdf(real y, real x, real a, real b) {
    vector[4] t;
    t[1] = -1.5;
    t[2] = -0.5 * square(y - a * x) - b;
    t[3] = -0.5 * square(y + b * x) - 0.2;
    t[4] = -2.0;
    if (a * x > 0.2) return log_sum_exp(t);
    return -square(y - b * x);
  }
}
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
  for (n in 1:N) target += mixed_lpdf(y[n] | x[n], a, b);
}
