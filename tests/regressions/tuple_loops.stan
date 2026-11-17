functions {
  tuple(real, vector) rescale(vector v, real s) {
    if (s > 0.95) return (s, v / s);
    for (i in 1:rows(v))
      if (v[i] > 3 * s) return (0.0, v);
    return (s, v * s);
  }
}
data {
  int N;
  array[N] tuple(real, int) obs;
  vector[N] y;
}
parameters {
  tuple(real, real<lower=0>) coef;
  real<lower=0> sigma;
  tuple(simplex[3], real) mix;
}
transformed parameters {
  tuple(real<lower=0>, vector[3]) scaled = rescale(mix.1, sigma);
}
model {
  array[N] tuple(real, real) fitted;
  for (n in 1:N) {
    fitted[n].1 = coef.1 + coef.2 * obs[n].1;
    fitted[n].2 = sigma * (1 + obs[n].2);
  }
  for (n in 1:N) y[n] ~ normal(fitted[n].1, fitted[n].2);
  tuple(real, real) shift = (1, coef.1);
  coef.1 ~ normal(0, 2);
  coef.2 ~ lognormal(0, 1);
  sigma ~ exponential(1);
  mix.2 ~ normal(shift.1 * scaled.1, shift.2 + 3);
  scaled.2 ~ normal(0, 1);
}
generated quantities {
  array[N] tuple(real, int) draws;
  for (n in 1:N)
    draws[n] = (normal_rng(coef.1 + coef.2 * obs[n].1, sigma), obs[n].2);
  real second = (normal_rng(0, 1), normal_rng(10, 1)).2;
  tuple(real, vector[3]) narrow = rescale(mix.1, 0.1);
}
