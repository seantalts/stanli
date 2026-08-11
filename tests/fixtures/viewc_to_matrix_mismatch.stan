parameters { vector[6] q; }
model {
  matrix[2, 4] M = to_matrix(q, 2, 4);
  target += sum(M);
}
