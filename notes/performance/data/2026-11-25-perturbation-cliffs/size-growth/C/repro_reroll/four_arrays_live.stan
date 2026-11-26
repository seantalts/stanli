data {
  int<lower=1> N;
  int<lower=1> T;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  array[N, T] real a;
  array[N, T] real b;
  array[N, T] real c;
  array[N, T] real y_pred;
  for (i in 1:N) {
    for (t in 1:T) {
      a[i, t] = 0;
      b[i, t] = 0;
      c[i, t] = 0;
      y_pred[i, t] = -1;
    }
  }
  for (i in 1:N) {
    a[i, 1] = mu;
    y_pred[i, 1] = normal_rng(mu, 1);
  }
}
