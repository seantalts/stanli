functions {
  real corner(matrix m) {
    array[rows(m), cols(m)] real a = to_array_2d(m);
    return a[2, 3];
  }
}
data {
  matrix[2, 3] d;
}
transformed data {
  array[2, 3] real ad = to_array_2d(d);
  real td_pick = ad[2, 1] + 10 * ad[1, 3];
}
parameters {
  matrix[2, 3] M;
}
transformed parameters {
  array[2, 3] real tp = to_array_2d(M);
}
model {
  array[2, 3] real a = to_array_2d(M);
  target += 10 * a[2, 1] + 100 * a[1, 2] + 1000 * a[1, 3];
  target += td_pick * tp[2, 2];
  target += 10000 * corner(M);
}
generated quantities {
  array[2, 3] real g = to_array_2d(M);
  real b = g[2, 1];
}
