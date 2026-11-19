functions {
  int outside_lp(real x) {
    x ~ normal(0, 1);
    return x < 0.5 || x > 2;
  }
}
parameters {
  real p;
}
transformed parameters {
  real t = outside_lp(p);
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  real g = t;
}
