data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real<lower=-1, upper=1> rho;
  real<lower=0> sigma;
}
model {
  real m = 0;
  // m carries a value from one iteration to the next.
  for (n in 1:N) {
    target += normal_lpdf(y[n] | rho * m, sigma);
    m = 0.5 * m + rho * y[n];
  }
}
