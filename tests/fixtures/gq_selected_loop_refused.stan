data {
  int<lower=0> N;
  vector[N] y;
}
parameters {
  real mu;
}
generated quantities {
  array[N] real draws;
  for (n in 1:N) {
    real step = 0;
    int k = 0;
    while (k < 2) {
      step += mu * y[n];
      k += 1;
    }
    if (step > 0) {
      draws[n] = normal_rng(mu, 1.0);
    } else {
      draws[n] = normal_rng(-mu, 1.0);
    }
  }
}
