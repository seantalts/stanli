data {
  int<lower=2> N;
  int<lower=2> L;
}
transformed data {
  array[L] vector[3] u;
  for (t in 1:L)
    for (k in 1:3)
      u[t][k] = sin(t + 2 * k);
}
parameters {
  vector[3] w;
}
model {
  array[N] vector[3] logA;
  for (t in 1:N)
    logA[t] = log_softmax(u[t] .* w);
  vector[3] alpha = logA[1];
  for (t in 2:L) {
    vector[3] next;
    for (j in 1:3)
      next[j] = log_sum_exp(alpha + logA[t]) + w[j] * u[t][j];
    alpha = next;
  }
  target += log_sum_exp(alpha);
}
