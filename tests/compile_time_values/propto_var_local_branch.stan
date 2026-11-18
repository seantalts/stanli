// A model-block local is a var in CmdStan even when its value is data, so
// normal_lupdf keeps the quadratic term and the branch is taken.
parameters {
  real mu;
}
model {
  real m = 0.5;
  real x = normal_lupdf(m | 0, 1);
  if (x < -0.1) {
    target += 1;
  }
  mu ~ normal(0, 1);
}
