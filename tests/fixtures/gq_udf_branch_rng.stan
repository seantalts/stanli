functions {
  int safe_rng(real mu, real phi) {
    if (mu < 0.97) {
      return 0;
    } else if (phi > 1.1) {
      return poisson_rng(mu);
    } else {
      real rate = gamma_rng(phi, phi / mu);
      return poisson_rng(rate);
    }
  }
  real clipped(real x, real hi) {
    if (x > hi) return hi;
    return x;
  }
  array[] int counts_rng(vector mu, real phi) {
    int t = num_elements(mu);
    array[t] int y;
    for (s in 1:t) y[s] = safe_rng(mu[s], phi);
    return y;
  }
}
data {
  int N;
}
parameters {
  vector<lower=0>[N] mu;
  real<lower=0> phi;
}
model {
  mu ~ exponential(1);
  phi ~ exponential(1);
}
generated quantities {
  array[N] int y = counts_rng(mu, phi);
  real top = clipped(phi, 1.05);
}
