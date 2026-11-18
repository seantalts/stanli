data {
  int<lower=1> N;
  int<lower=2> K;
  array[N] int<lower=1, upper=K> y;
  vector[N] x;
}
parameters {
  real b;
  ordered[K - 1] c;
}
model {
  vector[N] mu = x * b;
  for (n in 1 : N) {
    target += ordered_logistic_lpmf(y[n] | mu[n], c);
  }
  b ~ std_normal();
  c ~ normal(0, 3);
}
