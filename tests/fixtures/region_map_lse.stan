functions {
  real race_lpdf(real y, real x, real a, real b) {
    vector[5] t;
    for (j in 1:5)
      t[j] = -0.5 * square(y - a * x * j / 5.0) * exp(b) - 0.1 * j * b;
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
  for (n in 1:N) target += race_lpdf(y[n] | x[n], a, b);
}
