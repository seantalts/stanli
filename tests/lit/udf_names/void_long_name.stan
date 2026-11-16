// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL reached with 0.1
functions {
  void check_this_rather_long_function_name_that_runs_past_sixty_chars(real x, int n) {
    if (n > 0) check_this_rather_long_function_name_that_runs_past_sixty_chars(x, n - 1);
    else reject("reached with ", x);
  }
}
parameters {
  real y;
}
model {
  check_this_rather_long_function_name_that_runs_past_sixty_chars(y, 2);
  y ~ normal(0, 1);
}
