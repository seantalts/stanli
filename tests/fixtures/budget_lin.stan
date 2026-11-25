data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
  real<lower=0> sigma;
}
model {
  // Reroll folds this loop into a few vector ops.
  for (n in 1:N)
    y[n] ~ normal(a + b * x[n], sigma);
}
