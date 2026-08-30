// The first condition changes matrix values at runtime, but not their declared
// geometry.  The second condition must fold from the live-out's SlotInfo and
// must not become another parameter-dependent region.
data {
  int<lower=1> n;
}
parameters {
  real theta;
  matrix[n, 2] a;
  matrix[n, 2] b;
}
model {
  matrix[n, 2] x = a;
  if (theta > 0)
    x = b;
  if (rows(x) == n)
    target += theta;
  else
    target += 10 * theta;
}
