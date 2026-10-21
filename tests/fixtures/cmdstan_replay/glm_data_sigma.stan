data {
  int N;
  matrix[N, 1] X;
  vector[N] y;
  real<lower=0> sigma;
}
parameters {
  vector[1] b;
}
model {
  y ~ normal(X * b, sigma);
}
