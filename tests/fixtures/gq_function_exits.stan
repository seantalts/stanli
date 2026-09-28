functions {
  real selected_exit_rng(real x) {
    if (x > 0) return normal_rng(x, 1);
    while (x < -0.1) {
      if (x < -0.5) return normal_rng(x, 2);
      return uniform_rng(0, 1);
    }
    return normal_rng(0, 1);
  }
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real draw = 0;
  int i = 0;
  while (i < 1) {
    draw = selected_exit_rng(x);
    i += 1;
  }
  real after = normal_rng(0, 1);
}
