data {
  int N;
  array[N] tuple(real, int) obs;
  vector[N] y;
}
parameters {
  tuple(real, real<lower=0>) coef;
  real<lower=0> sigma;
}
model {
  array[N] tuple(real, real) fitted;
  for (n in 1:N) {
    fitted[n].1 = coef.1 + coef.2 * obs[n].1;
    fitted[n].2 = sigma * (1 + obs[n].2);
  }
  for (n in 1:N) y[n] ~ normal(fitted[n].1, fitted[n].2);
  coef.1 ~ normal(0, 2);
  coef.2 ~ lognormal(0, 1);
  sigma ~ exponential(1);
}
generated quantities {
  array[N] tuple(real, int) draws;
  for (n in 1:N)
    draws[n] = (normal_rng(coef.1 + coef.2 * obs[n].1, sigma), obs[n].2);
  real second = (normal_rng(0, 1), normal_rng(10, 1)).2;
}
