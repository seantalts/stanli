parameters {
  matrix[2, 2] a;
}
model {
  matrix[2, 2] e = add_diag(a, 0.5);
  target += e[1, 1] - 0.7 * e[2, 1] + 1.3 * e[1, 2] + 0.4 * e[2, 2];
}
