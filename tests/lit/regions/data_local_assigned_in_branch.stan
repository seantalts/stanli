// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// stanc marks x as data because every value assigned to it is data. The
// branch on a parameter assigns it at run time, so a later test of x has to
// read the branch's result and not the value x had before it.
parameters {
  real p;
}
model {
  real x = 0;
  if (p < 100) x = 3;
  if (x != 3) reject("got ", x);
  p ~ normal(0, 1);
}
