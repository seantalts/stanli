// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real x;
}
model {
  x ~ normal(0, 1);
  real r;
  int done = 0;
  for (pass in 1:1) {
    while (x < 10) {
      done = 1;
      r = -4 * x;
      break;
    }
    if (done) break;
    r = x + 7;
  }
  if (r != -4 * x) reject("r = ", r);
}
