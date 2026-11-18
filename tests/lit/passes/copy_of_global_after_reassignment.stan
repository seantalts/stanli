// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"x": 0.52, "y": 2.75}
// c holds the old value of a. stanc's copy propagation used to replace
// `b = c` with `b = a` after `a = y`, so b took the new value. Found by the
// differential fuzzer (seed 6375).
data {
  real x;
  real y;
}
parameters {
  real p;
}
transformed parameters {
  real a = x;
  real b;
  {
    real c = a;
    a = y;
    b = c;
  }
  if (b != x) reject("b is ", b, ", expected ", x);
}
model {
  p ~ normal(0, 1);
}
