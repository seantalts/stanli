// After inlining f, the loop over i is a runtime-control region whose only
// assignment outside itself is the inliner's early-return flag, an integer
// the region folds. Dead code elimination removes the use of the returned
// value, so the region has no live-out and must still compile.
functions {
  real f() {
    for (e in rep_array(0, 0)) {
      return 0.5;
    }
    return 0.5;
  }
}

parameters {
  real theta;
}

transformed parameters {
  for (i in 1:1) {
    real t = f();
  }
}

model {
  target += theta;
}
