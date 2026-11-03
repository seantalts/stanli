// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL segment: i is 0, but must be greater than 0
// STANLI-LIT-DATA: {"ts": [1, 2, 3]}
// CmdStan only runs the segment call when p > 10, so it returns values at
// every point the test uses. stanli evaluates it while lowering.
data {
  array[3] real ts;
}
parameters {
  real p;
}
model {
  if (p > 10) {
    array[0] real s = segment(ts, 0, 0);
    target += sum(s);
  }
  p ~ normal(0, 1);
}
