data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real ls;
}
model {
  vector[N] mu = a * x;
  real s = exp(ls);
  for (n in 1:N) {
    if (y[n] - mu[n] < -0.3)
      target += normal_lpdf(y[n] | mu[n], s);
    else
      target += student_t_lpdf(y[n] | 4, mu[n], s);
  }
}
