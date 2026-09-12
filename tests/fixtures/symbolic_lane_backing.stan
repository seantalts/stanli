data {
  int<lower=0> N;
  int<lower=N> M;
  array[M] real y;
  array[M] real unused;
}
parameters {
  real a;
  real b;
}
model {
  for (i in 1:N) {
    real z = exp(b * y[i]);
    target += z;
  }
}
