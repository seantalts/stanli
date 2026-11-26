data { int<lower=1> N; array[N] real y; }
parameters { simplex[2] t1; simplex[2] t2; positive_ordered[2] mu; }
model {
  array[2] vector[2] theta = {t1, t2};
  array[2] real acc; array[N, 2] real gamma;
  for (k in 1:2) gamma[1, k] = normal_lpdf(y[1] | mu[k], 1);
  for (t in 2:N) for (k in 1:2) {
    for (j in 1:2) acc[j] = gamma[t - 1, j] + log(theta[j, k]) + normal_lpdf(y[t] | mu[k], 1);
    gamma[t, k] = log_sum_exp(acc);
  }
  target += log_sum_exp(gamma[N]);
}
