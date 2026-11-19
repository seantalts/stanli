// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// The print makes the while loop a runtime region that writes w. A
// real-valued test of w inside it has to read the value w has on each pass,
// not the value it had before the loop.
parameters {
  real p;
}
model {
  int w = 0;
  int taken = 0;
  while (w < 3) {
    w += 1;
    print("w=", w);
    if (w * 0.5 > 0.7) taken += 1;
  }
  if (taken != 2) reject("taken ", taken);
  p ~ normal(0, 1);
}
