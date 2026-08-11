parameters {
  vector[4] q;
}
model {
  row_vector[2] a = [q[1], q[2]];
  row_vector[2] b = [q[3], q[4]];
  matrix[2, 2] M = append_row(a, b);
  target += M[1, 2] + 10 * M[2, 1];
}
