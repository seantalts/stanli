parameters { vector[2] v; }
model {
  matrix[3, 2] M = rep_matrix(0.0, 3, 2);
  M[:, 1] = v;
  target += sum(M);
}
