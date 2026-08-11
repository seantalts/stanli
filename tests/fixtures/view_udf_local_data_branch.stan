functions {
  real choose(real x, data array[,] real A) {
    real flag = 1.0 + 2.0;
    array[2, 3] real B = A;
    if (flag > 0.0 && B[1, 2] == 2.0) {
      return x;
    }
    return -x;
  }
  real wrap(real x, data array[,] real A) {
    return choose(x, A);
  }
}
data {
  array[2, 3] real A;
}
parameters {
  real q;
}
model {
  target += wrap(q, A);
}
