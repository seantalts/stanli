data {
  int<lower=0> N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  vector[2] acc = rep_vector(0, 2);
  for (n in 1:N) {
    int k = 1 + (mu <= 0);
    acc[k] += y[n] * mu;
  }
  target += acc[1] - 2 * acc[2];
}
