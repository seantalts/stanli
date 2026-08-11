model {
  matrix[0, 3] M = rep_matrix(0.0, 0, 3);
  target += sum(eigenvalues_sym(M));
}
