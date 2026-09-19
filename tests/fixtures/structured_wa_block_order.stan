data {
  int<lower=0> N;
  vector[N] y;
  int<lower=0> lim;
}
parameters {
  real mu;
}
transformed parameters {
  vector[N] z;
  {
    real acc = 0;
    while (acc < lim) acc += 1;
    for (n in 1:N) {
      real step;
      if (mu * y[n] > 0) {
        matrix[1100, 1000] big = rep_matrix(mu, 1100, 1000);
        step = big[1, 1] + acc;
      } else {
        step = -mu * y[n] + acc;
      }
      z[n] = step;
    }
    if (is_nan(acc)) z[1] = 0;
  }
}
model {
  mu ~ normal(0, 1);
  target += normal_lpdf(z[N] | 0, 1);
}
