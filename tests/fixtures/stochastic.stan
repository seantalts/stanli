parameters {
  column_stochastic_matrix[4, 3] c;
  row_stochastic_matrix[3, 4] r;
  array[2] row_stochastic_matrix[2, 3] a;
}
model {
  target += 2 * c[1, 1] + 3 * c[2, 3] + 4 * r[1, 2] + 5 * r[3, 4];
  target += 6 * a[1, 1, 2] + 7 * a[2, 2, 3];
}
