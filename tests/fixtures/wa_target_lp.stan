functions {
  real bump_lp(vector v) {
    target += 0.5;
    target += v;
    return target() + 1;
  }
}

parameters {
  real x;
}

transformed parameters {
  jacobian += x;
  real t = bump_lp([0.25, 0.125]');
  real u = bump_lp([0, 0]');
}

model {
  x ~ normal(0, 1);
}
