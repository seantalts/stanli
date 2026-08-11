functions {
  real twice(real x) {
    return 2 * x;
  }
}
parameters {
  real q;
}
model {
  if (q > 0) {
    target += twice(q);
  } else {
    target += -twice(q);
  }
}
