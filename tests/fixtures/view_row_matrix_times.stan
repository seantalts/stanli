parameters {
  vector[6] q;
}
model {
  row_vector[2] r = [q[1], q[2]];
  matrix[2, 2] M = to_matrix(q[3:6], 2, 2);
  row_vector[2] out = r * M;
  target += out[1] + 10 * out[2];
}
