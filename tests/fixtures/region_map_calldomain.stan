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
    real mu = a * x[n];
    if (y[n] - mu < -0.3)
      target += normal_lpdf(y[n] | mu, b * x[n]);
    else
      target += student_t_lpdf(y[n] | 4, mu, exp(b));
  }
}
