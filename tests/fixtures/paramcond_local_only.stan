// A parameter-dependent branch whose statements write only its own locals.
// Nothing leaves the branch, but the scale it computes is checked when the
// branch runs, so a nonpositive scale must still reject.
data {
  real s;
}
parameters {
  real theta;
}
model {
  if (theta > 0) {
    real x = normal_lpdf(1 | 0, s - theta);
  }
  target += -0.5 * theta ^ 2;
}
