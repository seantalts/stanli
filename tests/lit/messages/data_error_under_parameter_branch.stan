// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"ts": [1, 2, 3]}
// CmdStan only runs the segment call when p > 10, so it returns values at
// every point the test uses.
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
