parameters { row_vector[2] r; }
model {
  matrix[3, 2] M = rep_matrix(0.0, 3, 2);
  M[2] = r;
  target += M[2, 1] + 10 * M[2, 2];
}
