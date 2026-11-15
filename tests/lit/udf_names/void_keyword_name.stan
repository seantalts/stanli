// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL reached with 0.1
functions {
  void public(real x, int n) {
    if (n > 0) public(x, n - 1);
    else reject("reached with ", x);
  }
}
parameters {
  real y;
}
model {
  public(y, 2);
  y ~ normal(0, 1);
}
