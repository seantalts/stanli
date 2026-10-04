functions {
  real checked_lpdf(real y, real x, real mu, real sigma) {
    if (y > 10) return negative_infinity();
    return normal_lpdf(y | mu * x, sigma);
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  for (n in 1:N) target += checked_lpdf(y[n] | x[n], mu, sigma);
}
