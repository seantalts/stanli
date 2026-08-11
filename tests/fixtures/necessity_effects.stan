// Parameter-dependent branches with a numeric live-out and an effect.
// Without the target increment, lowering already refuses the region for
// producing nothing; the live-out is what exposed the legacy silent erasure.
data {
  int<lower=1, upper=2> mode;
}
parameters {
  real x;
}
model {
  if (mode == 1) {
    if (x > 0) {
      print("positive x=", x);
      target += x;
    } else {
      target += x;
    }
  } else {
    if (x < 0) {
      target += x;
      reject("negative x=", x);
    } else {
      target += x;
    }
  }
}
