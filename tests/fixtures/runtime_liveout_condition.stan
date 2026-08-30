data {
  real d;
}
parameters {
  real theta;
}
model {
  real x = d;
  if (theta > 0) {
    x = theta;
  }
  if (x > 0) {
    target += 10;
  } else {
    target += 20;
  }
  target += x;
}
