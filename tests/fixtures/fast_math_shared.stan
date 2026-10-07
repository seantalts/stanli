data {
  int<lower=0> N;
  vector[N] x;
  vector[N] y;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  for (n in 1:N) {
    target += normal_lpdf(y[n] | mu * x[n] + mu, sigma);
  }
}
