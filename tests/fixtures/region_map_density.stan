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
  real s = exp(0.4 * b);
  real nu = 2 + exp(0.2 * a);
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    if (r < b) {
      target += normal_lpdf(y[n] | a * x[n], s);
      target += std_normal_lpdf(r);
      target += exponential_lpdf(s | 1.5);
    } else {
      target += student_t_lpdf(y[n] | nu, a * x[n], s);
      target += normal_lpdf(y[n] | x[n], s);
      target += student_t_lpdf(r | nu, 0, s);
    }
  }
}
