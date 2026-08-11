functions {
  real poison(real y) {
    int z = 1;
    return z * y;
  }

  real wrap(real y) {
    real z = y;
    real a = poison(y);
    z = 3 * y;
    return a + z;
  }
}
parameters {
  real q;
}
model {
  int z = 7;
  target += wrap(q);
  target += z;
}
