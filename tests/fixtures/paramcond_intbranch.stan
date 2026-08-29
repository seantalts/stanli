// The same integer, assigned where the assignment may not run. The region
// carries it back as a runtime scalar: the taken path writes 2 and the
// untaken path preserves the pre-region value 1.
parameters {
  real theta;
}
model {
  int n = 1;
  real z = 0;
  if (theta > 0) {
    n = 2;
    z = theta * 5;
  }
  target += n * theta + z;
}
