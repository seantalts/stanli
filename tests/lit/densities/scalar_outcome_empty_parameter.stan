// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"y": 1, "N": 0}
data {
  int y;
  int N;
}
parameters {
  vector<lower=0, upper=1>[N] theta;
  real mu;
}
model {
  y ~ bernoulli(theta);
  y ~ poisson_log(rep_vector(mu, N));
  mu ~ normal(0, 1);
}
