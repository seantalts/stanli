data {
  vector[6] y;
  vector[6] z;
}
parameters {
  vector[6] mu;
}
model {
  array[3] vector[2] res;
  int i = 1;
  int j = 1;
  while (j <= 6) {
    target += normal_lpdf(y[j] | mu[j], 1);
    if (j > 100) target += normal_lpdf(z[j] | mu[j], 1);
    res[i,] = mu[j : (j + 1)];
    i += 1;
    j += 2;
  }
  for (r in 1 : 3) res[r] ~ normal(0, 1);
}
