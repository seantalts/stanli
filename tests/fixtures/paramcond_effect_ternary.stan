// A conditional expression whose condition calls a function that prints and
// can reject, without the O1 inliner. The call runs once per evaluation.
functions {
  real f(real x) {
    print("f ", x);
    if (x < -1) {
      reject("x is ", x);
    }
    return x;
  }
}
parameters {
  real p;
}
model {
  real y = f(p) > 0 ? p : -p;
  target += y;
}
