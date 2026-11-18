// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// The continue is taken at run time, so the break after it is never
// reached and the loop runs all three passes.
parameters {
  real p;
}
model {
  real n = 0;
  for (i in 1:3) {
    n += 1;
    if (p < 1000) continue;
    break;
  }
  if (n != 3) reject("n ", n);
  p ~ normal(0, 1);
}
