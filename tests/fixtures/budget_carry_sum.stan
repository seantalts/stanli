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
  // Large terms of both signs that nearly cancel, so the order in which
  // the target adds them shows in the last bits.
  for (n in 1:N) {
    target += normal_lpdf(y[n] | rho * m, sigma) + 1e6 * x[n] * (1 + rho);
    m = 0.5 * m + rho * y[n];
  }
}
