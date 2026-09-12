data {
  int<lower=0> N;
  array[N] real y;
}
parameters {
  real a;
  real b;
}
model {
  for (i in 1:N) {
    real z = exp(y[i] + b);
    target += z;
  }
}
