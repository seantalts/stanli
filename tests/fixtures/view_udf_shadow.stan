functions {
  real passthrough(real t) {
    array[2] matrix[2, 2] z;
    return t;
  }
}
parameters {
  array[2, 2] real z;
}
model {
  target += passthrough(z[1, 1]);
  target += z[1, 2];
}
