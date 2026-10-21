data {
  int N;
  vector[N] y;
  vector[N] mu;
  vector<lower=0>[N] sigma;
  real<lower=0> nu;
}
parameters {
  real a;
  real<lower=0> s;
}
model {
  y ~ normal(a, sigma);
  y ~ normal(mu, sigma);
  y ~ cauchy(mu, s);
  y ~ cauchy(a, sigma);
  y ~ student_t(nu, a, sigma);
  y ~ student_t(nu, mu, s);
}
