data {
  int<lower=1> N;
  array[N] int<lower=1, upper=7> actor;
  array[N] int<lower=1, upper=4> treatment;
  array[N] int<lower=0, upper=1> y;
}
parameters {
  vector[4] b;
  vector[7] a;
  real a_bar;
  real<lower=0> sigma_a;
}
model {
  vector[N] p;
  sigma_a ~ exponential(1);
  a_bar ~ normal(0, 1.5);
  a ~ normal(a_bar, sigma_a);
  b ~ normal(0, 0.5);
  for (i in 1 : N) {
    p[i] = a[actor[i]] + b[treatment[i]];
    p[i] = inv_logit(p[i]);
  }
  y ~ binomial(1, p);
}
