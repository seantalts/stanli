functions {
  void f_lp() {
    target += 0.5;
  }
}
parameters {
  real p;
}
transformed parameters {
  f_lp();
  real t = target();
}
model {
  p ~ normal(0, 1);
}
