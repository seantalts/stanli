data {
  int<lower=0> N;
  vector[N] y;
}
parameters {
  real mu;
}
transformed parameters {
  vector[N] z;
  for (n in 1:N) {
    real acc;
    if (mu * y[n] > 0) {
      matrix[1100, 1000] big = rep_matrix(mu, 1100, 1000);
      acc = big[1, 1] + y[n];
    } else {
      acc = -mu + y[n];
    }
    z[n] = acc;
  }
}
model {
  mu ~ normal(0, 1);
  target += normal_lpdf(z[N] | 0, 1);
}
generated quantities {
  array[N] real z_noisy;
  for (n in 1:N) z_noisy[n] = normal_rng(z[n], 1.0);
}
