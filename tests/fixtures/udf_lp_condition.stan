functions {
  real shift_lp(real y) {
    target += -0.5 * square(y);
    return y;
  }
  void bump_lp(real y) {
    target += y;
  }
}
parameters {
  real p;
}
model {
  real x = 0;
  if (shift_lp(p) < 0.2) x = 1;
  target += x;
  if (p < 1) {
    x = 2;
    bump_lp(p);
  }
  target += x;
}
