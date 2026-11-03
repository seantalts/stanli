data {
  int N;
  vector[N] y;
}
parameters {
  vector[N] mu;
}
transformed parameters {
  real s;
  {
    int k = 0;
    while (2 * (k + 1) <= N) k += 1;
    vector[k] z = mu[1 : k];
    s = sum(z);
  }
}
model {
  int m = 0;
  while (2 * (m + 1) <= N) m += 1;
  vector[m] z = mu[1 : m];
  z ~ normal(0, 1);
  s ~ normal(0, 1);
  y ~ normal(mu, 1);
}
