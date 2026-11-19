data {
  int<lower=1> G;
  int<lower=1> N;
  array[G] int<lower=1> size;
  array[G] int<lower=1> first;
  vector[N] y;
  vector[N] x;
}
parameters {
  vector[G] mu;
  real beta;
  real<lower=0> sigma;
}
model {
  for (g in 1:G) {
    array[size[g]] int rows
        = linspaced_int_array(size[g], first[g], first[g] + size[g] - 1);
    target += normal_lpdf(y[rows] | mu[g] + beta * x[rows], sigma);
  }
  mu ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma ~ lognormal(0, 1);
}
generated quantities {
  real fit = 0;
  for (g in 1:G) {
    array[size[g]] int rows
        = linspaced_int_array(size[g], first[g], first[g] + size[g] - 1);
    fit += sum(mu[g] + beta * x[rows]);
  }
}
