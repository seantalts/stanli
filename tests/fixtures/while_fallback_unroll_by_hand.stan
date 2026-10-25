data {
  vector[6] y;
  vector[6] z;
}
parameters {
  vector[6] mu;
}
model {
  array[3] vector[2] res;
  target += normal_lpdf(y[1] | mu[1], 1);
  res[1,] = mu[1 : 2];
  target += normal_lpdf(y[3] | mu[3], 1);
  res[2,] = mu[3 : 4];
  target += normal_lpdf(y[5] | mu[5], 1);
  res[3,] = mu[5 : 6];
  for (r in 1 : 3) res[r] ~ normal(0, 1);
}
