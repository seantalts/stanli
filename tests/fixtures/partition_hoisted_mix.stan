functions {
  real mix_lpdf(real y, real x, real mu, real sigma, real p) {
    return log_sum_exp(log(p) + normal_lpdf(y | 0, 3),
                       log1m(p) + normal_lpdf(y | mu * x, sigma));
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real mu;
  real<lower=0> sigma;
  real<lower=0, upper=1> p;
}
model {
  for (n in 1:N) target += mix_lpdf(y[n] | x[n], mu, sigma, p);
}
