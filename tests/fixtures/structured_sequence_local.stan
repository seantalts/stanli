data {
  int N;
  vector[N] y;
  array[3] int ks;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  array[1] int zero = {0};
  real s = 0;
  for (n in 1:N) {
    for (k in append_array(zero, ks)) s = 0.9 * s + 0.1 * k * y[n] - 0.05 * mu;
    target += normal_lpdf(y[n] | mu + s, sigma);
  }
  target += normal_lpdf(mu | 0, 2) + lognormal_lpdf(sigma | 0, 1);
}
