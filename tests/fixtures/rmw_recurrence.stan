data {
  int<lower=2> N;
  array[N] int<lower=1, upper=5> J;
  vector[N] Y;
}
parameters {
  real a0;
  real<lower=0> sigma;
  vector[5] r;
}
model {
  vector[N] mu;
  mu[1] = a0 + r[J[1]];
  for (n in 2 : N) {
    mu[n] = 0.5 * mu[n - 1] + r[J[n]];
  }
  a0 ~ normal(0, 1);
  sigma ~ student_t(3, 0, 2.5);
  r ~ normal(0, 1);
  Y ~ normal(mu, sigma);
}
