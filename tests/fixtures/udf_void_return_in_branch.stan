functions {
  void stop_above(real a) {
    if (a > 1) return;
  }
}
parameters {
  real p;
}
model {
  if (p >= -0.3) {
    stop_above(p);
    target += p;
  }
}
