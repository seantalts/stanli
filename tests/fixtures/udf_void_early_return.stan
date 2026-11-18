// A void function that returns early on a parameter, called on its own and
// under a parameter-dependent branch, without the O1 inliner. The first
// call rejects when p < -1, the second only when 0 < p < 1.
functions {
  void check(real x) {
    if (x > 1) {
      return;
    }
    if (x < -1) {
      reject("x is ", x);
    }
    return;
  }
}
parameters {
  real p;
}
model {
  check(p);
  if (p > 0) {
    check(p - 2);
  }
  target += p;
}
