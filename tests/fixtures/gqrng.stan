// Generated quantities combining caller-owned RNG with a runtime branch.  The
// branch is compiled as one structured program region, preserving the source
// order of both draws and the condition.
data {
  int<lower=1> N;
}
parameters {
  real<lower=0> sigma;
}
model {
  sigma ~ normal(0, 1);
}
generated quantities {
  real yrep = normal_rng(0, sigma);
  int crep = binomial_rng(N, 0.5);
  real branchy;
  if (sigma > 1) {
    branchy = 1;
  } else {
    branchy = 0;
  }
  real p = prod({1.0, 2.0, 3.0});
}
