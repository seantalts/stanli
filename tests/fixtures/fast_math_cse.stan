data {
  int<lower=0> N;
  array[N] int<lower=0, upper=1> y;
}
parameters {
  real<lower=0, upper=1> p;
  real<lower=0, upper=1> q;
}
model {
  for (n in 1:N) {
    if (y[n] == 1) {
      target += log(p) + bernoulli_lpmf(1 | q);
    } else {
      target += log1m(p) + bernoulli_lpmf(0 | q);
    }
  }
}
