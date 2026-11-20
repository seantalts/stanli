data {
  int<lower=1> G;
  int<lower=1> N;
  int<lower=1> K;
  array[G] int<lower=1> size;
  array[G] int<lower=1> first;
  vector[N] y;
  matrix[N, K] x;
}
parameters {
  vector[G] mu;
  vector[K] beta;
  real<lower=0> tau;
  real<lower=0> sigma;
}
transformed parameters {
  matrix[N, K] scaled = tau * x;
}
model {
  for (g in 1:G) {
    array[size[g]] int rows
        = linspaced_int_array(size[g], first[g], first[g] + size[g] - 1);
    target += normal_lpdf(y[rows] - x[rows] * beta | mu[g], sigma);
    target += normal_lpdf(y[rows] - scaled[rows] * beta | 0, 1);
  }
  mu ~ normal(0, 1);
  beta ~ normal(0, 1);
  tau ~ lognormal(0, 1);
  sigma ~ lognormal(0, 1);
}
generated quantities {
  real fit = 0;
  for (g in 1:G) {
    array[size[g]] int rows
        = linspaced_int_array(size[g], first[g], first[g] + size[g] - 1);
    fit += sum(mu[g] + x[rows] * beta) + sum(scaled[rows] * beta);
  }
}
