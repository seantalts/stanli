data {
  int N;
  vector[N] y;
  vector[N] x;
}
transformed data {
  real ybar = mean(y);
  vector[N] yc = y - ybar;
}
parameters {
  real<lower=0, upper=1> theta;
  ordered[2] mu;
  real<lower=0> sigma;
}
model {
  mu ~ normal(0, 3);
  sigma ~ exponential(1);
  for (n in 1:N)
    target += log_sum_exp(log(theta) + normal_lpdf(yc[n] | mu[1], sigma),
                          log1m(theta) + normal_lpdf(yc[n] | mu[2], sigma));
}
generated quantities {
  // Generated quantities read data and transformed data after the program
  // budget has lowered log_prob a second time.
  real spread = dot_product(x, yc) * theta + ybar;
  real last = y[N] + yc[1] * sigma;
}
