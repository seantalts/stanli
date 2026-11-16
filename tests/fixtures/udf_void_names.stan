functions {
  void public(real x) {
    if (x > 0) reject("public saw ", x);
  }
  void check_this_rather_long_function_name_that_runs_past_sixty_chars(real x) {
    if (x < 0) reject("long name saw ", x);
  }
}
parameters {
  real y;
}
model {
  public(y);
  check_this_rather_long_function_name_that_runs_past_sixty_chars(y);
  y ~ normal(0, 1);
}
